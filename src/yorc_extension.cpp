#define DUCKDB_EXTENSION_MAIN

//! DuckDB binding layer over the DuckDB-free core in src/core/:
//!   orcid_valid(id), orcid_normalize(id)   scalars
//!   orcid_<section>(path)                   one row per item, from XML or JSON files (format per document)
//!   orcid_<section>_xml(xml)                same, from an XML string (lateral)
//!   orcid_<section>_json(json)              same, from a JSON string (lateral)
//! for every section in orcid::Sections() (works, fundings, affiliations, ...,
//! the API messages: summary, summary_items, search_results, errors,
//! notifications, group_ids; and research_resource_hosts); and the writers:
//!   orcid_<writer>_to_xml(rec [, crosswalk]), orcid_<writer>_to_json(rec [, crosswalk])
//!                                           for every orcid::WriterNames()
//!   orcid_bulk_works_to_xml(list [, crosswalk]), orcid_bulk_works_to_json(list [, crosswalk])
//!   orcid_dump_sample(archive, n, seed)     the records yorc-rs's / yorc-zig's reservoir picks
//! and the setting orcid_crosswalk (SET orcid_crosswalk = true: the writers emit
//! yorc-rs's bytes; the argument wins over it). No network code: no access to
//! the ORCID Sandbox or the ORCID Member API, no requests to ORCID.
//! Version differences (v1.5.x vs v2.0) go through orcid/compat.hpp only.

#include "yorc_extension.hpp"

#include "orcid/changes.hpp"
#include "orcid/compat.hpp"
#include "orcid/id.hpp"
#include "orcid/json.hpp"
#include "orcid/json_map.hpp"
#include "orcid/reservoir.hpp"
#include "orcid/sections.hpp"
#include "orcid/tar.hpp"
#include "orcid/writer.hpp"
#include "orcid/xml.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/execution/partition_info.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

namespace duckdb {

// ---------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------

static void OrcidValidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(),
	                                       [](string_t id) { return orcid::IsValidId(id.GetString()); });
}

static void OrcidNormalizeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	OrcidUnaryWithNulls<string_t, string_t>(args.data[0], result, args.size(), [&](string_t id, bool &is_null) {
		std::string canonical;
		if (!orcid::NormalizeId(id.GetString(), canonical)) {
			is_null = true;
			return string_t();
		}
		return StringVector::AddString(result, canonical);
	});
}

// ---------------------------------------------------------------------------
// Section schema and row output
// ---------------------------------------------------------------------------

static LogicalType ToLogicalType(orcid::ColType type) {
	switch (type) {
	case orcid::ColType::Integer:
		return LogicalType::INTEGER;
	case orcid::ColType::Bigint:
		return LogicalType::BIGINT;
	case orcid::ColType::Boolean:
		return LogicalType::BOOLEAN;
	default:
		return LogicalType::VARCHAR;
	}
}

static void SectionSchema(const orcid::SectionSpec &spec, vector<LogicalType> &types, OrcidBindNames &names) {
	for (auto &col : spec.columns) {
		names.emplace_back(string(col.name));
		types.push_back(ToLogicalType(col.type));
	}
}

//! Writes one row. With `projection` (projection pushdown), output column i
//! holds section column projection[i]; ids past the section's columns (row id,
//! the v2.0 multi-file framework's virtual columns) are NULL here; the framework
//! fills its own.
static void WriteRow(DataChunk &out, idx_t row, const orcid::SectionSpec &spec, const orcid::Row &values,
                     const vector<column_t> *projection = nullptr) {
	const idx_t n = MinValue<idx_t>(projection ? projection->size() : spec.columns.size(), out.ColumnCount());
	for (idx_t i = 0; i < n; i++) {
		const idx_t c = projection ? (*projection)[i] : i;
		if (c >= spec.columns.size()) {
			FlatVector::SetNull(out.data[i], row, true);
			continue;
		}
		auto &vec = out.data[i];
		auto &value = values[c];
		bool valid = value.has_value();
		switch (spec.columns[c].type) {
		case orcid::ColType::Varchar:
			if (valid) {
				OrcidFlatData<string_t>(vec)[row] = StringVector::AddString(vec, *value);
			}
			break;
		case orcid::ColType::Integer: {
			auto parsed = valid ? orcid::ParseInteger(*value, 9) : std::optional<int64_t>();
			valid = parsed.has_value();
			if (valid) {
				OrcidFlatData<int32_t>(vec)[row] = static_cast<int32_t>(*parsed);
			}
			break;
		}
		case orcid::ColType::Bigint: {
			auto parsed = valid ? orcid::ParseInteger(*value, 18) : std::optional<int64_t>();
			valid = parsed.has_value();
			if (valid) {
				OrcidFlatData<int64_t>(vec)[row] = *parsed;
			}
			break;
		}
		case orcid::ColType::Boolean: {
			auto parsed = valid ? orcid::ParseBoolean(*value) : std::optional<bool>();
			valid = parsed.has_value();
			if (valid) {
				OrcidFlatData<bool>(vec)[row] = *parsed;
			}
			break;
		}
		}
		if (!valid) {
			FlatVector::SetNull(vec, row, true);
		}
	}
}

//! One document of either format into rows; core errors become DuckDB errors
//! naming the document.
static void ExtractOrThrow(const orcid::SectionSpec &spec, const char *data, size_t len, orcid::DocFormat format,
                           const string &json_root, const string &what, vector<orcid::Row> &rows) {
	try {
		orcid::ExtractDocument(spec, data, len, format, json_root, rows);
	} catch (const orcid::XmlError &e) {
		throw InvalidInputException("orcid: malformed XML in %s: %s", what, string(e.what()));
	} catch (const orcid::JsonError &e) {
		throw InvalidInputException("orcid: malformed JSON in %s: %s", what, string(e.what()));
	} catch (const orcid::JsonRootError &e) {
		throw InvalidInputException("orcid: cannot read the JSON in %s: %s", what, string(e.what()));
	}
}

//! `json_root`: the document element given to JSON documents whose content does
//! not name it (an XML local name such as 'employment' or 'keyword').
static string JsonRootProblem(const string &root, const string &function) {
	if (root.empty()) {
		return function + ": json_root must not be empty";
	}
	for (char c : root) {
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
			return function + ": json_root must be an ORCID element name such as 'employment' or 'keyword', got '" +
			       root + "'";
		}
	}
	return string();
}

static string JsonRootParameter(const Value &value, const string &function) {
	auto root = value.GetValue<string>();
	auto problem = JsonRootProblem(root, function);
	if (!problem.empty()) {
		throw BinderException("%s", problem);
	}
	return root;
}

//! json_root given as an input column. DuckDB v1.5.6 passes every argument of
//! an in-out function called on a lateral column (`f(t.doc, json_root := 'x')`)
//! as an input column, named or not, so the _json twins also take it as a
//! second (changes: third) argument, read per row. NULL means none.
static string JsonRootColumn(DataChunk &input, idx_t column, idx_t row, const string &fallback,
                             const string &function) {
	if (input.ColumnCount() <= column) {
		return fallback;
	}
	auto &vec = input.data[column];
	if (!FlatVector::Validity(vec).RowIsValid(row)) {
		return fallback;
	}
	auto root = FlatVector::GetData<string_t>(vec)[row].GetString();
	auto problem = JsonRootProblem(root, function);
	if (!problem.empty()) {
		throw InvalidInputException("%s", problem);
	}
	return root;
}

//! Archive members read when `members` is not given: every .xml and .json member.
static bool MemberSelected(const string &members, bool members_set, const string &name) {
	if (members_set) {
		return orcid::GlobMatch(members, name);
	}
	return orcid::GlobMatch("*.xml", name) || orcid::GlobMatch("*.json", name);
}

// ---------------------------------------------------------------------------
// orcid_<section>(path): one file at a time, so memory is bounded by the
// largest record. On v2.0 the multi-file framework wraps this and hands it one
// file; on v1.5.x it expands globs itself (see OrcidRegisterReader).
// ---------------------------------------------------------------------------

//! Largest archive member read (default; `max_member_bytes` raises or lowers
//! it). A member's content is held in memory whole while it is parsed, outside
//! DuckDB's memory_limit, so a crafted or corrupt archive (a gzip bomb, a
//! multi-GB member) must not be inflated blindly. The largest record in the
//! Public Data File corpus is 5.4 MB, so 64 MiB leaves a wide margin.
static constexpr int64_t DEFAULT_MAX_MEMBER_BYTES = int64_t(64) << 20;

static int64_t MaxMemberBytesParameter(const Value &value, const string &function) {
	auto n = value.GetValue<int64_t>();
	if (n < 1) {
		throw BinderException("%s: max_member_bytes must be >= 1", function);
	}
	return n;
}

//! Refuses an archive member larger than the limit, before any of its content
//! is read (the size comes from its header). Same policy as other unreadable
//! input: the scan fails with an error naming the archive and the member.
static void CheckMemberSize(const orcid::TarMember &member, int64_t max_member_bytes, const string &archive_path) {
	if (member.size > static_cast<uint64_t>(max_member_bytes)) {
		throw IOException("orcid: member '%s' in '%s' is %llu bytes, over max_member_bytes (%llu); "
		                  "raise max_member_bytes to read it",
		                  member.name, archive_path, static_cast<unsigned long long>(member.size),
		                  static_cast<unsigned long long>(max_member_bytes));
	}
}

struct FilesBindData : public TableFunctionData {
	const orcid::SectionSpec *spec = nullptr;
	vector<string> files;
	//! Archive member filter (glob over member paths; unset: *.xml and *.json),
	//! per-file record limit, the largest member read, and the JSON root.
	string members;
	bool members_set = false;
	string json_root;
	int64_t max_records = -1;
	int64_t max_member_bytes = DEFAULT_MAX_MEMBER_BYTES;
	//! Rough row estimate (FilesCardinality). DuckDB weighs each pipeline's
	//! share of the progress bar by its source's estimated cardinality, so
	//! without it the scan counted as little as the tiny pipelines after it.
	idx_t estimated_rows = 1;
};

//! Shared by every thread of one scan. Hands out documents one at a time:
//! a plain file by name (the thread reads and parses it outside the lock), an
//! archive member by content (the tar stream is sequential, so the member is
//! read under the lock; parsing still happens outside it).
struct FilesState : public GlobalTableFunctionState {
	mutex lock;
	idx_t next_file = 0;
	//! Batches handed out so far. A batch is a run of consecutive documents and
	//! its sequence number is its batch index, so DuckDB can restore document
	//! order (preserve_insertion_order).
	idx_t batches = 0;
	//! Open archive being streamed, if any.
	unique_ptr<FileHandle> archive;
	unique_ptr<orcid::TarReader> tar;
	string archive_path;
	int64_t records_in_file = 0;
	idx_t max_threads = 1;
	//! Documents per batch (see ClaimedBatch).
	idx_t batch_documents = 1;
	//! Projected section columns, in output order (projection pushdown).
	vector<column_t> column_ids;

	//! Progress (FilesProgress), written under `lock` by the claiming thread and
	//! read without it by DuckDB's progress bar: atomics only, no syscalls.
	//! files_started counts files handed out (plain files) or opened (archives);
	//! for the open archive, its compressed bytes consumed and total size.
	std::atomic<idx_t> files_started {0};
	std::atomic<bool> archive_open {false};
	std::atomic<uint64_t> archive_done {0};
	std::atomic<uint64_t> archive_total {0};
	//! Highest value reported so far, so the bar never moves backwards.
	mutable std::atomic<double> progress_reported {0};

	idx_t MaxThreads() const override {
		return max_threads;
	}
};

//! Per thread: the rows of the document being emitted.
struct FilesLocalState : public LocalTableFunctionState {
	vector<orcid::Row> rows;
	idx_t next_row = 0;
	idx_t batch_index = 0;
};

//! One document claimed from FilesState.
struct ClaimedDocument {
	string path;    //!< plain file to read, or
	string content; //!< archive member already read
	bool from_archive = false;
	string what;    //!< for error messages
};

//! Consecutive documents claimed together. Batches of several documents keep
//! the ordered sinks (e.g. an insert under preserve_insertion_order) from
//! merging one tiny batch per document, which otherwise dominates the scan.
struct ClaimedBatch {
	vector<ClaimedDocument> docs;
	idx_t batch_index = 0;
};
//! A batch ends after this many documents or, for archive members (read under
//! the lock), this many content bytes.
static constexpr idx_t BATCH_DOCUMENTS = 32;
static constexpr idx_t BATCH_ARCHIVE_BYTES = 4 << 20;

static bool IsArchive(const string &path) {
	auto lower = StringUtil::Lower(path);
	return StringUtil::EndsWith(lower, ".tar") || StringUtil::EndsWith(lower, ".tar.gz") ||
	       StringUtil::EndsWith(lower, ".tgz");
}

//! How to open an archive: DuckDB's AUTO_DETECT knows `.gz` but not `.tgz`,
//! which it would read as an uncompressed tar ("bad tar header checksum").
static FileCompressionType ArchiveCompression(const string &path) {
	return StringUtil::EndsWith(StringUtil::Lower(path), ".tgz") ? FileCompressionType::GZIP
	                                                              : FileCompressionType::AUTO_DETECT;
}

//! Whole file through DuckDB's file system; .gz is decompressed transparently.
static string ReadWholeFile(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileCompressionType::AUTO_DETECT);
	string out;
	char buf[64 * 1024];
	while (true) {
		auto n = handle->Read(buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		out.append(buf, static_cast<size_t>(n));
	}
	return out;
}

static vector<string> ExpandPaths(ClientContext &context, const string &pattern) {
	vector<string> paths;
	if (!OrcidReaderGlobsItself() || !FileSystem::HasGlob(pattern)) {
		paths.push_back(pattern);
		return paths;
	}
	auto &fs = FileSystem::GetFileSystem(context);
	for (auto &info : fs.GlobFiles(pattern, FileGlobOptions::DISALLOW_EMPTY)) {
		paths.push_back(info.path);
	}
	std::sort(paths.begin(), paths.end());
	return paths;
}

template <size_t I>
static unique_ptr<FunctionData> FilesBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, OrcidBindNames &names) {
	auto &spec = orcid::Sections()[I];
	SectionSchema(spec, return_types, names);
	if (input.inputs[0].IsNull()) {
		throw BinderException("orcid_%s: path must not be NULL", spec.name);
	}
	auto result = make_uniq<FilesBindData>();
	result->spec = &spec;
	result->files = ExpandPaths(context, input.inputs[0].GetValue<string>());
	for (auto &kv : input.named_parameters) {
		auto key = StringUtil::Lower(OrcidNameString(kv.first));
		if (kv.second.IsNull()) {
			continue;
		}
		if (key == "members") {
			result->members = kv.second.GetValue<string>();
			result->members_set = true;
		} else if (key == "max_records") {
			result->max_records = kv.second.GetValue<int64_t>();
			if (result->max_records < 0) {
				throw BinderException("orcid_%s: max_records must be >= 0", spec.name);
			}
		} else if (key == "max_member_bytes") {
			result->max_member_bytes = MaxMemberBytesParameter(kv.second, "orcid_" + spec.name);
		} else if (key == "json_root") {
			result->json_root = JsonRootParameter(kv.second, "orcid_" + spec.name);
		}
	}
	// Estimate: ~1 row per 2 KB of a (compressed) archive, read with one size
	// lookup per archive; 50 rows per plain file (no per-file syscalls), capped
	// by max_records.
	auto &fs = FileSystem::GetFileSystem(context);
	double rows = 0;
	for (auto &path : result->files) {
		double file_rows = 50;
		if (IsArchive(path)) {
			try {
				auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileCompressionType::UNCOMPRESSED);
				file_rows = MaxValue(50.0, static_cast<double>(handle->GetFileSize()) / 2048.0);
			} catch (const std::exception &) {
				file_rows = 1e6; // unknown size (e.g. a stream): assume a large dump
			}
			if (result->max_records >= 0) {
				file_rows = MinValue(file_rows, 50.0 * static_cast<double>(result->max_records));
			}
		} else if (result->max_records == 0) {
			file_rows = 0;
		}
		rows += file_rows;
	}
	result->estimated_rows = static_cast<idx_t>(MaxValue(1.0, MinValue(rows, 1e15)));
	return std::move(result);
}

static unique_ptr<NodeStatistics> FilesCardinality(ClientContext &context, const FunctionData *bind_data) {
	auto rows = bind_data->Cast<FilesBindData>().estimated_rows;
	return make_uniq<NodeStatistics>(rows, rows);
}

static unique_ptr<GlobalTableFunctionState> FilesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<FilesBindData>();
	auto state = make_uniq<FilesState>();
	state->column_ids = input.column_ids;
	// Plain files: one thread per file at most. An archive's members are spread
	// over threads too, so with an archive the scan may use every thread; DuckDB
	// caps this at its `threads` setting either way.
	bool any_archive = false;
	for (auto &path : bind.files) {
		any_archive = any_archive || IsArchive(path);
	}
	// Batches of up to BATCH_DOCUMENTS documents, but small enough that a glob
	// of a few files still gives every thread several batches.
	const idx_t threads = MaxValue<idx_t>(1, TaskScheduler::GetScheduler(context).NumberOfThreads());
	state->batch_documents =
	    any_archive ? BATCH_DOCUMENTS : MaxValue<idx_t>(1, MinValue<idx_t>(BATCH_DOCUMENTS, bind.files.size() / (threads * 8)));
	state->max_threads = any_archive ? GlobalTableFunctionState::MAX_THREADS
	                                 : MaxValue<idx_t>(1, (bind.files.size() + state->batch_documents - 1) /
	                                                          state->batch_documents);
	return std::move(state);
}

static unique_ptr<LocalTableFunctionState> FilesInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                          GlobalTableFunctionState *global_state) {
	return make_uniq<FilesLocalState>();
}

//! Claims the next document; the caller holds the lock. Returns false when
//! every file and every archive member has been handed out. max_records is
//! counted here, per file, so it is exact however many threads scan.
static bool ClaimDocumentLocked(ClientContext &context, const FilesBindData &bind, FilesState &state,
                                ClaimedDocument &doc) {
	auto &fs = FileSystem::GetFileSystem(context);
	while (true) {
		if (state.tar) {
			orcid::TarMember member;
			bool found = false;
			if (bind.max_records < 0 || state.records_in_file < bind.max_records) {
				try {
					while (state.tar->Next(member)) {
						if (MemberSelected(bind.members, bind.members_set, member.name)) {
							found = true;
							break;
						}
					}
				} catch (const orcid::TarError &e) {
					throw IOException("orcid: %s in '%s'", string(e.what()), state.archive_path);
				}
			}
			if (found) {
				CheckMemberSize(member, bind.max_member_bytes, state.archive_path);
				try {
					doc.content = state.tar->ReadContent();
				} catch (const orcid::TarError &e) {
					throw IOException("orcid: %s in '%s'", string(e.what()), state.archive_path);
				}
				state.records_in_file++;
				doc.from_archive = true;
				doc.what = "'" + state.archive_path + "' member '" + member.name + "'";
				return true;
			}
			// Archive exhausted, or max_records reached: stop reading it here.
			state.archive_open = false;
			state.tar.reset();
			state.archive.reset();
			continue;
		}
		if (state.next_file >= bind.files.size()) {
			return false;
		}
		const auto &path = bind.files[state.next_file++];
		state.records_in_file = 0;
		if (IsArchive(path)) {
			state.archive_path = path;
			state.archive = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | ArchiveCompression(path));
			auto *handle = state.archive.get();
			// Progress: compressed bytes consumed out of the file's size (one
			// size lookup per archive). For a plain .tar the bytes read are the
			// bytes consumed. For .tar.gz, the position of the underlying file is
			// asked at most once per MiB of decompressed data (an lseek locally,
			// in-memory for httpfs); CompressedFile::GetProgress() is not used
			// because it counts buffered input repeatedly and runs ahead.
			auto *gz = dynamic_cast<CompressedFile *>(handle);
			const bool compressed = handle->GetFileCompressionType() != FileCompressionType::UNCOMPRESSED;
			int64_t size = 0;
			try {
				size = handle->GetFileSize();
			} catch (const std::exception &) {
				size = 0; // unknown (e.g. a stream): this archive then counts as a whole file
			}
			state.archive_total = size > 0 ? static_cast<uint64_t>(size) : 0;
			state.archive_done = 0;
			state.archive_open = true;
			state.files_started = state.next_file;
			auto *st = &state;
			auto since_update = std::make_shared<uint64_t>(0);
			state.tar = make_uniq<orcid::TarReader>([handle, gz, compressed, st, since_update](char *buf, size_t n) -> size_t {
				auto got = handle->Read(buf, n);
				if (got > 0) {
					if (!compressed) {
						st->archive_done = st->archive_done.load() + static_cast<uint64_t>(got);
					} else if (gz && gz->child_handle && (*since_update += static_cast<uint64_t>(got)) >= (1u << 20)) {
						*since_update = 0;
						try {
							st->archive_done = static_cast<uint64_t>(gz->child_handle->SeekPosition());
						} catch (const std::exception &) {
							// position unknown (a stream): this archive counts as a whole file
						}
					}
				}
				return got > 0 ? static_cast<size_t>(got) : 0;
			});
			continue;
		}
		state.files_started = state.next_file;
		if (bind.max_records == 0) {
			continue;
		}
		doc.path = path;
		doc.from_archive = false;
		doc.what = "'" + path + "'";
		return true;
	}
}

//! Claims the next batch of documents. Returns false when nothing is left.
static bool ClaimBatch(ClientContext &context, const FilesBindData &bind, FilesState &state, ClaimedBatch &batch) {
	lock_guard<mutex> guard(state.lock);
	batch.docs.clear();
	idx_t bytes = 0;
	while (batch.docs.size() < state.batch_documents && bytes < BATCH_ARCHIVE_BYTES) {
		ClaimedDocument doc;
		if (!ClaimDocumentLocked(context, bind, state, doc)) {
			break;
		}
		bytes += doc.content.size();
		batch.docs.push_back(std::move(doc));
	}
	if (batch.docs.empty()) {
		return false;
	}
	batch.batch_index = state.batches++;
	return true;
}

//! Each output chunk holds rows of one batch only, so that its batch index
//! describes the whole chunk.
static void FilesScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<FilesBindData>();
	auto &state = data.global_state->Cast<FilesState>();
	auto &local = data.local_state->Cast<FilesLocalState>();

	while (local.next_row >= local.rows.size()) {
		local.rows.clear();
		local.next_row = 0;
		ClaimedBatch batch;
		if (!ClaimBatch(context, bind, state, batch)) {
			OrcidSetCardinality(output, 0);
			return;
		}
		local.batch_index = batch.batch_index;
		// The format is told per document (orcid::DetectFormat), so a glob or an
		// archive may mix XML and JSON.
		for (auto &doc : batch.docs) {
			if (!doc.from_archive) {
				doc.content = ReadWholeFile(FileSystem::GetFileSystem(context), doc.path);
			}
			const auto format = orcid::DetectFormat(doc.content.data(), doc.content.size());
			ExtractOrThrow(*bind.spec, doc.content.data(), doc.content.size(), format, bind.json_root, doc.what,
			               local.rows);
			doc.content.clear();
		}
	}
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && local.next_row < local.rows.size()) {
		WriteRow(output, count++, *bind.spec, local.rows[local.next_row++], &state.column_ids);
	}
	OrcidSetCardinality(output, count);
}

//! Percentage done: every file weighs the same; the open archive counts by the
//! share of its compressed bytes consumed. Plain files count when handed out.
//! Monotonic, in [0, 100]; cheap (atomic loads only).
static double FilesProgress(ClientContext &context, const FunctionData *bind_data_p,
                            const GlobalTableFunctionState *global_state) {
	auto &bind = bind_data_p->Cast<FilesBindData>();
	auto &state = global_state->Cast<FilesState>();
	const double n = static_cast<double>(bind.files.size());
	if (n == 0) {
		return 100.0;
	}
	const bool open = state.archive_open.load();
	double done = static_cast<double>(state.files_started.load());
	if (open) {
		const auto total = state.archive_total.load();
		const double frac =
		    total > 0 ? MinValue(1.0, static_cast<double>(state.archive_done.load()) / static_cast<double>(total)) : 0.0;
		done = MaxValue(0.0, done - 1.0) + frac;
	}
	double pct = MinValue(100.0, MaxValue(0.0, 100.0 * done / n));
	double prev = state.progress_reported.load();
	while (pct > prev && !state.progress_reported.compare_exchange_weak(prev, pct)) {
	}
	return MaxValue(pct, prev);
}

static OperatorPartitionData FilesPartitionData(ClientContext &context, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("orcid: partition columns are not supported");
	}
	return OperatorPartitionData(input.local_state->Cast<FilesLocalState>().batch_index);
}

// ---------------------------------------------------------------------------
// orcid_<section>_xml(xml) / orcid_<section>_json(json): in-out functions, so
// they work on literals and on columns via lateral joins:
//   SELECT w.* FROM records r, orcid_works_xml(r.xml) w
// A NULL input yields zero rows with the full schema. The _xml twin reads XML
// only and the _json twin JSON only (no detection: a string column is one
// format); `json_root` as for the file readers.
// ---------------------------------------------------------------------------

struct XmlBindData : public TableFunctionData {
	const orcid::SectionSpec *spec = nullptr;
	orcid::DocFormat format = orcid::DocFormat::Xml;
	string json_root;
};

struct XmlLocalState : public LocalTableFunctionState {
	idx_t next_input = 0;
	vector<orcid::Row> rows;
	idx_t next_row = 0;
};

template <size_t I>
static unique_ptr<FunctionData> XmlBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, OrcidBindNames &names) {
	auto &spec = orcid::Sections()[I];
	SectionSchema(spec, return_types, names);
	auto result = make_uniq<XmlBindData>();
	result->spec = &spec;
	return std::move(result);
}

template <size_t I>
static unique_ptr<FunctionData> JsonBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, OrcidBindNames &names) {
	auto &spec = orcid::Sections()[I];
	SectionSchema(spec, return_types, names);
	auto result = make_uniq<XmlBindData>();
	result->spec = &spec;
	result->format = orcid::DocFormat::Json;
	for (auto &kv : input.named_parameters) {
		if (StringUtil::Lower(OrcidNameString(kv.first)) == "json_root" && !kv.second.IsNull()) {
			result->json_root = JsonRootParameter(kv.second, "orcid_" + spec.name + "_json");
		}
	}
	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> XmlInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                        GlobalTableFunctionState *global_state) {
	return make_uniq<XmlLocalState>();
}

static OperatorResultType XmlInOut(ExecutionContext &context, TableFunctionInput &data, DataChunk &input,
                                   DataChunk &output) {
	auto &bind = data.bind_data->Cast<XmlBindData>();
	auto &spec = *bind.spec;
	auto &state = data.local_state->Cast<XmlLocalState>();
	input.Flatten();
	auto &xml_vec = input.data[0];
	auto xml_data = FlatVector::GetData<string_t>(xml_vec);
	auto &validity = FlatVector::Validity(xml_vec);

	// Protocol (as json_each): a call that produces rows returns HAVE_MORE_OUTPUT;
	// only a call that produces nothing returns NEED_MORE_INPUT and resets.
	// Both callers rely on it: the in-out operator (lateral / subquery input)
	// fetches the next input on NEED_MORE_INPUT, while a constant argument is
	// driven by PhysicalTableScan, which ignores the result and calls again with
	// the same input until a call returns zero rows.
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (state.next_row < state.rows.size()) {
			WriteRow(output, count++, spec, state.rows[state.next_row++]);
			continue;
		}
		if (state.next_input >= input.size()) {
			break;
		}
		idx_t i = state.next_input++;
		state.rows.clear();
		state.next_row = 0;
		if (validity.RowIsValid(i)) {
			auto doc = xml_data[i];
			const string root = JsonRootColumn(input, 1, i, bind.json_root, "orcid_" + spec.name + "_json");
			ExtractOrThrow(spec, doc.GetData(), doc.GetSize(), bind.format, root, "input row", state.rows);
		}
	}
	OrcidSetCardinality(output, count);
	if (count == 0) {
		state.next_input = 0;
		state.rows.clear();
		state.next_row = 0;
		return OperatorResultType::NEED_MORE_INPUT;
	}
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

// ---------------------------------------------------------------------------
// orcid_changes(old_path, new_path) / orcid_changes_xml(old_xml, new_xml) /
// orcid_changes_json(old_json, new_json): differences between two snapshots
// of a record (see orcid::DiffDocuments). The file form tells the format of
// each file, so an XML snapshot can be compared with a JSON one.
// ---------------------------------------------------------------------------

static void ChangesSchema(vector<LogicalType> &types, OrcidBindNames &names) {
	auto add = [&](const char *name, const LogicalType &type) {
		names.emplace_back(string(name));
		types.push_back(type);
	};
	add("orcid", LogicalType::VARCHAR);
	add("section", LogicalType::VARCHAR);
	add("kind", LogicalType::VARCHAR);
	add("put_code", LogicalType::BIGINT);
	add("change", LogicalType::VARCHAR);
	add("field", LogicalType::VARCHAR);
	add("old_value", LogicalType::VARCHAR);
	add("new_value", LogicalType::VARCHAR);
	add("label", LogicalType::VARCHAR);
}

static void SetOptional(Vector &vec, idx_t row, const std::optional<std::string> &value) {
	if (value) {
		OrcidFlatData<string_t>(vec)[row] = StringVector::AddString(vec, *value);
	} else {
		FlatVector::SetNull(vec, row, true);
	}
}

static void WriteChange(DataChunk &out, idx_t row, const orcid::ChangeRow &c) {
	SetOptional(out.data[0], row, c.orcid.empty() ? std::optional<std::string>() : std::optional<std::string>(c.orcid));
	SetOptional(out.data[1], row, c.section);
	SetOptional(out.data[2], row, c.kind.empty() ? std::optional<std::string>() : std::optional<std::string>(c.kind));
	auto put = c.put_code ? orcid::ParseInteger(*c.put_code, 18) : std::optional<int64_t>();
	if (put) {
		OrcidFlatData<int64_t>(out.data[3])[row] = *put;
	} else {
		FlatVector::SetNull(out.data[3], row, true);
	}
	SetOptional(out.data[4], row, c.change);
	SetOptional(out.data[5], row, c.field);
	SetOptional(out.data[6], row, c.old_value);
	SetOptional(out.data[7], row, c.new_value);
	SetOptional(out.data[8], row, c.label);
}

static void DiffOrThrow(const string &old_doc, orcid::DocFormat old_format, const string &new_doc,
                        orcid::DocFormat new_format, const string &json_root, const string &what,
                        vector<orcid::ChangeRow> &out) {
	try {
		orcid::DiffDocuments(old_doc.data(), old_doc.size(), old_format, new_doc.data(), new_doc.size(), new_format,
		                     json_root, out);
	} catch (const orcid::XmlError &e) {
		throw InvalidInputException("orcid: malformed XML in %s: %s", what, string(e.what()));
	} catch (const orcid::JsonError &e) {
		throw InvalidInputException("orcid: malformed JSON in %s: %s", what, string(e.what()));
	} catch (const orcid::JsonRootError &e) {
		throw InvalidInputException("orcid: cannot read the JSON in %s: %s", what, string(e.what()));
	}
}

//! json_root from a changes function's named parameters ("" when absent).
static string ChangesJsonRoot(TableFunctionBindInput &input, const string &function) {
	string root;
	for (auto &kv : input.named_parameters) {
		if (StringUtil::Lower(OrcidNameString(kv.first)) == "json_root" && !kv.second.IsNull()) {
			root = JsonRootParameter(kv.second, function);
		}
	}
	return root;
}

struct ChangesFilesBindData : public TableFunctionData {
	string old_path, new_path;
	string json_root;
};

struct ChangesFilesState : public GlobalTableFunctionState {
	bool computed = false;
	vector<orcid::ChangeRow> rows;
	idx_t next_row = 0;
};

static unique_ptr<FunctionData> ChangesFilesBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, OrcidBindNames &names) {
	ChangesSchema(return_types, names);
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("orcid_changes: paths must not be NULL");
	}
	auto result = make_uniq<ChangesFilesBindData>();
	result->old_path = input.inputs[0].GetValue<string>();
	result->new_path = input.inputs[1].GetValue<string>();
	result->json_root = ChangesJsonRoot(input, "orcid_changes");
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ChangesFilesInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ChangesFilesState>();
}

static void ChangesFilesScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<ChangesFilesBindData>();
	auto &state = data.global_state->Cast<ChangesFilesState>();
	if (!state.computed) {
		auto &fs = FileSystem::GetFileSystem(context);
		const string old_doc = ReadWholeFile(fs, bind.old_path);
		const string new_doc = ReadWholeFile(fs, bind.new_path);
		DiffOrThrow(old_doc, orcid::DetectFormat(old_doc.data(), old_doc.size()), new_doc,
		            orcid::DetectFormat(new_doc.data(), new_doc.size()), bind.json_root,
		            "'" + bind.old_path + "' / '" + bind.new_path + "'", state.rows);
		state.computed = true;
	}
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.next_row < state.rows.size()) {
		WriteChange(output, count++, state.rows[state.next_row++]);
	}
	OrcidSetCardinality(output, count);
}

struct ChangesXmlLocalState : public LocalTableFunctionState {
	idx_t next_input = 0;
	vector<orcid::ChangeRow> rows;
	idx_t next_row = 0;
};

struct ChangesDocBindData : public TableFunctionData {
	orcid::DocFormat format = orcid::DocFormat::Xml;
	string json_root;
};

static unique_ptr<FunctionData> ChangesXmlBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, OrcidBindNames &names) {
	ChangesSchema(return_types, names);
	return make_uniq<ChangesDocBindData>();
}

static unique_ptr<FunctionData> ChangesJsonBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, OrcidBindNames &names) {
	ChangesSchema(return_types, names);
	auto result = make_uniq<ChangesDocBindData>();
	result->format = orcid::DocFormat::Json;
	result->json_root = ChangesJsonRoot(input, "orcid_changes_json");
	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> ChangesXmlInitLocal(ExecutionContext &context,
                                                               TableFunctionInitInput &input,
                                                               GlobalTableFunctionState *global_state) {
	return make_uniq<ChangesXmlLocalState>();
}

//! Same protocol as XmlInOut: HAVE_MORE_OUTPUT whenever rows were produced,
//! NEED_MORE_INPUT (and reset) only from a call that produced none.
static OperatorResultType ChangesXmlInOut(ExecutionContext &context, TableFunctionInput &data, DataChunk &input,
                                          DataChunk &output) {
	auto &bind = data.bind_data->Cast<ChangesDocBindData>();
	auto &state = data.local_state->Cast<ChangesXmlLocalState>();
	input.Flatten();
	auto old_data = FlatVector::GetData<string_t>(input.data[0]);
	auto new_data = FlatVector::GetData<string_t>(input.data[1]);
	auto &old_valid = FlatVector::Validity(input.data[0]);
	auto &new_valid = FlatVector::Validity(input.data[1]);

	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (state.next_row < state.rows.size()) {
			WriteChange(output, count++, state.rows[state.next_row++]);
			continue;
		}
		if (state.next_input >= input.size()) {
			break;
		}
		idx_t i = state.next_input++;
		state.rows.clear();
		state.next_row = 0;
		// NULL on one side means "no snapshot": everything on the other side is
		// added/removed. The empty side is an empty XML element in either twin.
		const bool has_old = old_valid.RowIsValid(i);
		const bool has_new = new_valid.RowIsValid(i);
		string old_doc = has_old ? old_data[i].GetString() : string("<empty/>");
		string new_doc = has_new ? new_data[i].GetString() : string("<empty/>");
		if (has_old || has_new) {
			const string root = JsonRootColumn(input, 2, i, bind.json_root, "orcid_changes_json");
			DiffOrThrow(old_doc, has_old ? bind.format : orcid::DocFormat::Xml, new_doc,
			            has_new ? bind.format : orcid::DocFormat::Xml, root, "input row", state.rows);
		}
	}
	OrcidSetCardinality(output, count);
	if (count == 0) {
		state.next_input = 0;
		state.rows.clear();
		state.next_row = 0;
		return OperatorResultType::NEED_MORE_INPUT;
	}
	return OperatorResultType::HAVE_MORE_OUTPUT;
}


// ---------------------------------------------------------------------------
// orcid_documents(path): the raw documents (XML or JSON) of files, globs and
// archive members, one row each (file, member, name, orcid, xml, format). Lets
// two dump releases be paired by record (e.g. on `orcid`) and compared with
// orcid_changes_xml without unpacking them. Sequential; `members` and
// `max_records` as for the section readers. `xml` keeps its name for either
// format: it is the document's text as read.
// ---------------------------------------------------------------------------

//! The first valid ORCID iD written as dddd-dddd-dddd-dddX inside `text`.
static string IdInPath(const string &text) {
	for (size_t i = 0; i + 19 <= text.size(); i++) {
		if (text[i + 4] != '-' || text[i + 9] != '-' || text[i + 14] != '-') {
			continue;
		}
		std::string id;
		if (orcid::NormalizeId(text.substr(i, 19), id) && orcid::IsValidId(text.substr(i, 19))) {
			return id;
		}
	}
	return string();
}

static string BaseName(const string &path) {
	auto slash = path.find_last_of("/\\");
	return slash == string::npos ? path : path.substr(slash + 1);
}

struct DocumentsBindData : public TableFunctionData {
	vector<string> files;
	string members;
	bool members_set = false;
	int64_t max_records = -1;
	int64_t max_member_bytes = DEFAULT_MAX_MEMBER_BYTES;
};

struct DocumentsState : public GlobalTableFunctionState {
	idx_t next_file = 0;
	unique_ptr<FileHandle> archive;
	unique_ptr<orcid::TarReader> tar;
	string archive_path;
	int64_t records_in_file = 0;
	uint64_t archive_total = 0;
	std::atomic<uint64_t> archive_done {0};
	std::atomic<idx_t> files_started {0};
};

static unique_ptr<FunctionData> DocumentsBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, OrcidBindNames &names) {
	for (const char *n : {"file", "member", "name", "orcid", "xml", "format"}) {
		names.emplace_back(string(n));
		return_types.push_back(LogicalType::VARCHAR);
	}
	if (input.inputs[0].IsNull()) {
		throw BinderException("orcid_documents: path must not be NULL");
	}
	auto result = make_uniq<DocumentsBindData>();
	const string pattern = input.inputs[0].GetValue<string>();
	auto &fs = FileSystem::GetFileSystem(context);
	if (FileSystem::HasGlob(pattern)) {
		for (auto &info : fs.GlobFiles(pattern, FileGlobOptions::DISALLOW_EMPTY)) {
			result->files.push_back(info.path);
		}
		std::sort(result->files.begin(), result->files.end());
	} else {
		result->files.push_back(pattern);
	}
	for (auto &kv : input.named_parameters) {
		auto key = StringUtil::Lower(OrcidNameString(kv.first));
		if (kv.second.IsNull()) {
			continue;
		}
		if (key == "members") {
			result->members = kv.second.GetValue<string>();
			result->members_set = true;
		} else if (key == "max_records") {
			result->max_records = kv.second.GetValue<int64_t>();
			if (result->max_records < 0) {
				throw BinderException("orcid_documents: max_records must be >= 0");
			}
		} else if (key == "max_member_bytes") {
			result->max_member_bytes = MaxMemberBytesParameter(kv.second, "orcid_documents");
		}
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> DocumentsInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DocumentsState>();
}

static double DocumentsProgress(ClientContext &context, const FunctionData *bind_data_p,
                                const GlobalTableFunctionState *global_state) {
	auto &bind = bind_data_p->Cast<DocumentsBindData>();
	auto &state = global_state->Cast<DocumentsState>();
	if (bind.files.empty()) {
		return 100.0;
	}
	double done = static_cast<double>(state.files_started.load());
	if (state.tar && state.archive_total > 0) {
		done = MaxValue(0.0, done - 1.0) +
		       MinValue(1.0, static_cast<double>(state.archive_done.load()) / static_cast<double>(state.archive_total));
	}
	return MinValue(100.0, 100.0 * done / static_cast<double>(bind.files.size()));
}

static void DocumentsScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<DocumentsBindData>();
	auto &state = data.global_state->Cast<DocumentsState>();
	auto &fs = FileSystem::GetFileSystem(context);
	idx_t count = 0;
	auto emit = [&](const string &file, const string *member, const string &xml) {
		const string &where = member ? *member : file;
		// `xml` is a VARCHAR: refuse bytes that are not UTF-8 (the section readers
		// do the same, in orcid::ExtractSection and the JSON parser)
		for (const string *text : {&xml, member}) {
			if (!text) {
				continue;
			}
			const size_t valid = orcid::Utf8ValidPrefix(text->data(), text->size());
			if (valid != text->size()) {
				throw InvalidInputException("orcid_documents: invalid UTF-8 at byte %llu of %s",
				                            static_cast<uint64_t>(valid),
				                            text == member ? "a member name in " + file : where);
			}
		}
		OrcidFlatData<string_t>(output.data[0])[count] = StringVector::AddString(output.data[0], file);
		if (member) {
			OrcidFlatData<string_t>(output.data[1])[count] = StringVector::AddString(output.data[1], *member);
		} else {
			FlatVector::SetNull(output.data[1], count, true);
		}
		OrcidFlatData<string_t>(output.data[2])[count] = StringVector::AddString(output.data[2], BaseName(where));
		auto id = IdInPath(where);
		if (id.empty()) {
			FlatVector::SetNull(output.data[3], count, true);
		} else {
			OrcidFlatData<string_t>(output.data[3])[count] = StringVector::AddString(output.data[3], id);
		}
		OrcidFlatData<string_t>(output.data[4])[count] = StringVector::AddString(output.data[4], xml);
		const bool json = orcid::DetectFormat(xml.data(), xml.size()) == orcid::DocFormat::Json;
		OrcidFlatData<string_t>(output.data[5])[count] = StringVector::AddString(output.data[5], json ? "json" : "xml");
		count++;
	};
	while (count < STANDARD_VECTOR_SIZE) {
		if (state.tar) {
			orcid::TarMember member;
			bool found = false;
			if (bind.max_records < 0 || state.records_in_file < bind.max_records) {
				try {
					while (state.tar->Next(member)) {
						if (MemberSelected(bind.members, bind.members_set, member.name)) {
							found = true;
							break;
						}
					}
				} catch (const orcid::TarError &e) {
					throw IOException("orcid: %s in '%s'", string(e.what()), state.archive_path);
				}
			}
			if (!found) {
				state.tar.reset();
				state.archive.reset();
				continue;
			}
			CheckMemberSize(member, bind.max_member_bytes, state.archive_path);
			string xml;
			try {
				xml = state.tar->ReadContent();
			} catch (const orcid::TarError &e) {
				throw IOException("orcid: %s in '%s'", string(e.what()), state.archive_path);
			}
			state.records_in_file++;
			emit(state.archive_path, &member.name, xml);
			continue;
		}
		if (state.next_file >= bind.files.size()) {
			break;
		}
		const auto &path = bind.files[state.next_file++];
		state.files_started = state.next_file;
		state.records_in_file = 0;
		if (IsArchive(path)) {
			state.archive_path = path;
			state.archive = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | ArchiveCompression(path));
			auto *handle = state.archive.get();
			auto *gz = dynamic_cast<CompressedFile *>(handle);
			const bool compressed = handle->GetFileCompressionType() != FileCompressionType::UNCOMPRESSED;
			int64_t size = 0;
			try {
				size = handle->GetFileSize();
			} catch (const std::exception &) {
				size = 0;
			}
			state.archive_total = size > 0 ? static_cast<uint64_t>(size) : 0;
			state.archive_done = 0;
			auto *st = &state;
			auto since = std::make_shared<uint64_t>(0);
			state.tar = make_uniq<orcid::TarReader>([handle, gz, compressed, st, since](char *buf, size_t n) -> size_t {
				auto got = handle->Read(buf, n);
				if (got > 0) {
					if (!compressed) {
						st->archive_done = st->archive_done.load() + static_cast<uint64_t>(got);
					} else if (gz && gz->child_handle && (*since += static_cast<uint64_t>(got)) >= (1u << 20)) {
						*since = 0;
						try {
							st->archive_done = static_cast<uint64_t>(gz->child_handle->SeekPosition());
						} catch (const std::exception &) {
						}
					}
				}
				return got > 0 ? static_cast<size_t>(got) : 0;
			});
			continue;
		}
		if (bind.max_records == 0) {
			continue;
		}
		emit(path, nullptr, ReadWholeFile(fs, path));
	}
	OrcidSetCardinality(output, count);
}

// ---------------------------------------------------------------------------
// orcid_dump_sample(path, n, seed): a seeded reservoir sample of the records
// of a Public Data File archive, the same members yorc-rs's dump::Reservoir
// and yorc-zig's reservoir pick (orcid/reservoir.hpp: SplitMix64, Algorithm R,
// the eligibility rule of their walkthroughs and bench programs). One pass,
// sequential. A member is read only while it is in the sample and only when
// the `xml` column is asked for (projection pushdown); skipped members are
// never read, as in the libraries. A new function: the section readers and
// orcid_documents are unchanged.
// ---------------------------------------------------------------------------

static constexpr idx_t SAMPLE_XML_COLUMN = 5;

struct DumpSampleBindData : public TableFunctionData {
	vector<string> files;
	uint64_t n = 0;
	uint64_t seed = 0;
	int64_t max_member_bytes = DEFAULT_MAX_MEMBER_BYTES;
};

struct SampledMember {
	string orcid;
	string file;
	string member;
	string xml;
};

struct DumpSampleState : public GlobalTableFunctionState {
	bool computed = false;
	vector<SampledMember> kept; //!< in slot order
	uint64_t eligible = 0;
	idx_t next_row = 0;
	vector<column_t> column_ids;
	bool need_xml = false;
};

static unique_ptr<FunctionData> DumpSampleBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, OrcidBindNames &names) {
	auto add = [&](const char *name, const LogicalType &type) {
		names.emplace_back(string(name));
		return_types.push_back(type);
	};
	add("slot", LogicalType::BIGINT);
	add("orcid", LogicalType::VARCHAR);
	add("file", LogicalType::VARCHAR);
	add("member", LogicalType::VARCHAR);
	add("name", LogicalType::VARCHAR);
	add("xml", LogicalType::VARCHAR); // SAMPLE_XML_COLUMN
	add("eligible", LogicalType::BIGINT);
	for (idx_t i = 0; i < 3; i++) {
		if (input.inputs[i].IsNull()) {
			throw BinderException("orcid_dump_sample: path, n and seed must not be NULL");
		}
	}
	auto result = make_uniq<DumpSampleBindData>();
	const auto n = input.inputs[1].GetValue<int64_t>();
	if (n < 0) {
		throw BinderException("orcid_dump_sample: n must be >= 0");
	}
	result->n = static_cast<uint64_t>(n);
	// seed: UBIGINT, or BIGINT (an integer literal binds there: DuckDB does not
	// cast INTEGER to UBIGINT implicitly)
	if (input.inputs[2].type().id() == LogicalTypeId::UBIGINT) {
		result->seed = input.inputs[2].GetValue<uint64_t>();
	} else {
		const auto seed = input.inputs[2].GetValue<int64_t>();
		if (seed < 0) {
			throw BinderException("orcid_dump_sample: seed must be >= 0 (or a UBIGINT)");
		}
		result->seed = static_cast<uint64_t>(seed);
	}
	const string pattern = input.inputs[0].GetValue<string>();
	auto &fs = FileSystem::GetFileSystem(context);
	if (FileSystem::HasGlob(pattern)) {
		for (auto &info : fs.GlobFiles(pattern, FileGlobOptions::DISALLOW_EMPTY)) {
			result->files.push_back(info.path);
		}
		std::sort(result->files.begin(), result->files.end());
	} else {
		result->files.push_back(pattern);
	}
	for (auto &path : result->files) {
		if (!IsArchive(path)) {
			throw BinderException("orcid_dump_sample: '%s' is not a .tar, .tar.gz or .tgz archive", path);
		}
	}
	for (auto &kv : input.named_parameters) {
		if (StringUtil::Lower(OrcidNameString(kv.first)) == "max_member_bytes" && !kv.second.IsNull()) {
			result->max_member_bytes = MaxMemberBytesParameter(kv.second, "orcid_dump_sample");
		}
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> DumpSampleInit(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<DumpSampleState>();
	state->column_ids = input.column_ids;
	for (auto c : input.column_ids) {
		state->need_xml = state->need_xml || c == SAMPLE_XML_COLUMN;
	}
	return std::move(state);
}

//! The one pass: every member of every archive, in order.
static void DumpSampleCompute(ClientContext &context, const DumpSampleBindData &bind, DumpSampleState &state) {
	auto &fs = FileSystem::GetFileSystem(context);
	orcid::ReservoirSampler sampler(bind.n, bind.seed);
	state.kept.reserve(static_cast<size_t>(MinValue<uint64_t>(bind.n, uint64_t(1) << 20)));
	for (auto &path : bind.files) {
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | ArchiveCompression(path));
		auto *h = handle.get();
		orcid::TarReader tar([h](char *buf, size_t n) -> size_t {
			auto got = h->Read(buf, n);
			return got > 0 ? static_cast<size_t>(got) : 0;
		});
		orcid::TarMember member;
		try {
			while (tar.Next(member)) {
				string id;
				if (!orcid::DumpSampleEligible(member.name, id)) {
					continue;
				}
				const orcid::Slot slot = sampler.Offer();
				if (slot.kind == orcid::SlotKind::Skip) {
					continue; // never read: Next() skips its bytes
				}
				SampledMember kept;
				kept.orcid = std::move(id);
				kept.file = path;
				kept.member = member.name;
				if (state.need_xml) {
					CheckMemberSize(member, bind.max_member_bytes, path);
					kept.xml = tar.ReadContent();
				}
				if (slot.kind == orcid::SlotKind::Append) {
					state.kept.push_back(std::move(kept));
				} else {
					state.kept[static_cast<size_t>(slot.index)] = std::move(kept);
				}
			}
		} catch (const orcid::TarError &e) {
			throw IOException("orcid: %s in '%s'", string(e.what()), path);
		}
	}
	state.eligible = sampler.Seen();
}

static void DumpSampleScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<DumpSampleBindData>();
	auto &state = data.global_state->Cast<DumpSampleState>();
	if (!state.computed) {
		DumpSampleCompute(context, bind, state);
		state.computed = true;
	}
	auto set_text = [&](Vector &vec, idx_t row, const string &text, const string &what) {
		const size_t valid = orcid::Utf8ValidPrefix(text.data(), text.size());
		if (valid != text.size()) {
			throw InvalidInputException("orcid_dump_sample: invalid UTF-8 at byte %llu of %s",
			                            static_cast<uint64_t>(valid), what);
		}
		OrcidFlatData<string_t>(vec)[row] = StringVector::AddString(vec, text);
	};
	idx_t count = 0;
	const idx_t ncols = MinValue<idx_t>(state.column_ids.size(), output.ColumnCount());
	while (count < STANDARD_VECTOR_SIZE && state.next_row < state.kept.size()) {
		const idx_t slot = state.next_row++;
		const auto &m = state.kept[slot];
		const string where = "member '" + m.member + "' of '" + m.file + "'";
		for (idx_t i = 0; i < ncols; i++) {
			auto &vec = output.data[i];
			switch (state.column_ids[i]) {
			case 0:
				OrcidFlatData<int64_t>(vec)[count] = static_cast<int64_t>(slot);
				break;
			case 1:
				OrcidFlatData<string_t>(vec)[count] = StringVector::AddString(vec, m.orcid);
				break;
			case 2:
				OrcidFlatData<string_t>(vec)[count] = StringVector::AddString(vec, m.file);
				break;
			case 3:
				set_text(vec, count, m.member, "a member name in '" + m.file + "'");
				break;
			case 4:
				set_text(vec, count, BaseName(m.member), "a member name in '" + m.file + "'");
				break;
			case SAMPLE_XML_COLUMN:
				set_text(vec, count, m.xml, where);
				break;
			case 6:
				OrcidFlatData<int64_t>(vec)[count] = static_cast<int64_t>(state.eligible);
				break;
			default: // row id and other virtual columns
				FlatVector::SetNull(vec, count, true);
				break;
			}
		}
		count++;
	}
	OrcidSetCardinality(output, count);
}

// ---------------------------------------------------------------------------
// Writers: orcid_<writer>_to_xml(rec) / orcid_<writer>_to_json(rec) for a
// STRUCT and orcid_bulk_works_to_xml / _json(list) for a LIST of them.
// The input becomes an orcid::WriteRecord (scalar fields as text, nested
// LIST(STRUCT) fields as lists) and the core serializes it (writer.cpp).
// Field names are checked against the writer's shape once per call, so a typo
// is an error rather than a silently missing element.
// ---------------------------------------------------------------------------

static bool IsNestedType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::UNION:
		return true;
	default:
		return false;
	}
}

static string ListFieldNames(const orcid::RecordShape &shape) {
	string out;
	for (auto &l : shape.lists) {
		out += (out.empty() ? "" : ", ") + l.first;
	}
	return out.empty() ? string("none") : out;
}

static void CheckShape(const LogicalType &type, const orcid::RecordShape &shape, const string &function,
                       const string &where);

//! One field (a STRUCT child) against the shape: a list field
//! must be a LIST of STRUCT of the list's shape, any other field a known
//! scalar (a column of the shape's reader section).
static void CheckField(const string &name, const LogicalType &type, const orcid::RecordShape &shape,
                       const string &function, const string &where) {
	if (auto *list = shape.ListShape(name)) {
		if (type.id() == LogicalTypeId::SQLNULL) {
			return;
		}
		if (type.id() != LogicalTypeId::LIST) {
			throw InvalidInputException("%s: '%s%s' must be a LIST of STRUCT, got %s", function, where, name,
			                            type.ToString());
		}
		auto &item = ListType::GetChildType(type);
		if (item.id() != LogicalTypeId::SQLNULL) {
			CheckShape(item, *list, function, where + name + "[].");
		}
		return;
	}
	if (!shape.AcceptsField(name)) {
		throw InvalidInputException("%s: unknown field '%s%s': the fields are the columns of orcid_%s, and the lists "
		                            "%s",
		                            function, where, name, shape.section, ListFieldNames(shape));
	}
	if (IsNestedType(type)) {
		throw InvalidInputException("%s: '%s%s' must be a scalar, got %s", function, where, name, type.ToString());
	}
}

static void CheckShape(const LogicalType &type, const orcid::RecordShape &shape, const string &function,
                       const string &where) {
	if (type.id() != LogicalTypeId::STRUCT) {
		throw InvalidInputException("%s: %s must be a STRUCT, got %s", function,
		                            where.empty() ? string("the argument") : "the items of '" + where + "'",
		                            type.ToString());
	}
	for (auto &child : StructType::GetChildTypes(type)) {
		CheckField(OrcidNameString(child.first), child.second, shape, function, where);
	}
}

static orcid::WriteRecord ToRecord(const Value &value, const orcid::RecordShape &shape);

//! Adds one non-NULL field to `record`: a list of records, or the value as text.
static void AddValue(orcid::WriteRecord &record, const string &name, const Value &value,
                     const orcid::RecordShape &shape) {
	if (value.IsNull()) {
		return;
	}
	if (auto *list = shape.ListShape(name)) {
		auto &items = record.lists[name]; // an empty list is given, unlike a NULL one
		for (auto &item : ListValue::GetChildren(value)) {
			if (!item.IsNull()) {
				items.push_back(ToRecord(item, *list));
			}
		}
		return;
	}
	record.fields[name] = value.ToString();
}

static orcid::WriteRecord ToRecord(const Value &value, const orcid::RecordShape &shape) {
	orcid::WriteRecord record;
	auto &types = StructType::GetChildTypes(value.type());
	auto &children = StructValue::GetChildren(value);
	for (idx_t c = 0; c < children.size() && c < types.size(); c++) {
		AddValue(record, OrcidNameString(types[c].first), children[c], shape);
	}
	return record;
}

//! The global crosswalk switch (docs/REFERENCE.md, "Crosswalk mode"): off by
//! default; `SET orcid_crosswalk = true` makes every writer emit yorc-rs's
//! bytes. A writer's trailing BOOLEAN argument wins over it.
static const char *const CROSSWALK_SETTING = "orcid_crosswalk";

static orcid::WriteStyle StyleOf(bool crosswalk) {
	return crosswalk ? orcid::WriteStyle::Crosswalk : orcid::WriteStyle::Native;
}

//! Runs `write` (Value, style -> document) on every row; NULL in, NULL out.
//! With a second argument (the per-call crosswalk switch) its value decides
//! the style of each row, and a NULL there gives NULL; without one, the
//! orcid_crosswalk setting does.
template <class FUNC>
static void WriteRows(DataChunk &args, ExpressionState &state, Vector &result, const string &function, FUNC write) {
	auto &input = args.data[0];
	const idx_t count = args.size();
	const bool by_argument = args.ColumnCount() > 1;
	const orcid::WriteStyle setting =
	    by_argument ? orcid::WriteStyle::Native : StyleOf(OrcidBooleanSetting(state.GetContext(), CROSSWALK_SETTING));
	auto out = OrcidFlatData<string_t>(result);
	for (idx_t i = 0; i < count; i++) {
		const Value value = input.GetValue(i);
		bool is_null = value.IsNull();
		orcid::WriteStyle style = setting;
		if (by_argument) {
			const Value crosswalk = args.data[1].GetValue(i);
			if (crosswalk.IsNull()) {
				is_null = true;
			} else {
				style = StyleOf(BooleanValue::Get(crosswalk.DefaultCastAs(LogicalType::BOOLEAN)));
			}
		}
		if (is_null) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		string doc;
		try {
			doc = write(value, style);
		} catch (const orcid::WriteError &e) {
			throw InvalidInputException("%s: %s", function, string(e.what()));
		}
		out[i] = StringVector::AddString(result, doc);
	}
}

template <size_t W, bool JSON>
static void WriterFun(DataChunk &args, ExpressionState &state, Vector &result) {
	const string &writer = orcid::WriterNames()[W];
	const string function = "orcid_" + writer + (JSON ? "_to_json" : "_to_xml");
	const orcid::RecordShape &shape = *orcid::WriterShape(writer);
	auto &type = args.data[0].GetType();
	if (type.id() != LogicalTypeId::SQLNULL) {
		CheckShape(type, shape, function, string());
	}
	const auto format = JSON ? orcid::WriteFormat::Json : orcid::WriteFormat::Xml;
	WriteRows(args, state, result, function, [&](const Value &value, orcid::WriteStyle style) {
		return orcid::WriteDocument(writer, ToRecord(value, shape), format, style);
	});
}

template <bool JSON>
static void BulkWorksFun(DataChunk &args, ExpressionState &state, Vector &result) {
	const string function = JSON ? "orcid_bulk_works_to_json" : "orcid_bulk_works_to_xml";
	const orcid::RecordShape &shape = *orcid::WriterShape("work");
	auto &type = args.data[0].GetType();
	if (type.id() != LogicalTypeId::SQLNULL) {
		if (type.id() != LogicalTypeId::LIST) {
			throw InvalidInputException("%s: the argument must be a LIST of work STRUCTs, got %s", function,
			                            type.ToString());
		}
		auto &item = ListType::GetChildType(type);
		if (item.id() != LogicalTypeId::SQLNULL) {
			CheckShape(item, shape, function, "works[].");
		}
	}
	const auto format = JSON ? orcid::WriteFormat::Json : orcid::WriteFormat::Xml;
	WriteRows(args, state, result, function, [&](const Value &value, orcid::WriteStyle style) {
		vector<orcid::WriteRecord> works;
		for (auto &work : ListValue::GetChildren(value)) {
			if (!work.IsNull()) {
				works.push_back(ToRecord(work, shape));
			}
		}
		return orcid::WriteBulkWorks(works, format, style);
	});
}

//! The (rec) and (rec, crosswalk BOOLEAN) overloads of a writer.
template <class FUN>
static void RegisterWriterScalar(ExtensionLoader &loader, const string &name, FUN fun) {
	// The return type is built here, not passed to emplace_back as
	// LogicalType::VARCHAR: binding that static constexpr member to the forwarding
	// reference odr-uses it, and the linker then sees a second definition of
	// duckdb::LogicalType::VARCHAR next to libduckdb_static's.
	const LogicalType varchar(LogicalTypeId::VARCHAR);
	vector<ScalarFunction> overloads;
	overloads.emplace_back(OrcidName(name), vector<LogicalType> {LogicalType::ANY}, varchar, fun);
	overloads.emplace_back(OrcidName(name), vector<LogicalType> {LogicalType::ANY, LogicalType::BOOLEAN}, varchar,
	                       fun);
	// Fallible: a record missing a required field raises an error.
	OrcidRegisterScalarSet(loader, name, std::move(overloads), true);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

//! Must equal orcid::Sections().size(); checked at load.
static constexpr size_t SECTION_COUNT = 19;

template <size_t I>
static void RegisterSection(ExtensionLoader &loader) {
	const string base = "orcid_" + orcid::Sections()[I].name;

	TableFunction files(OrcidName(base), {LogicalType::VARCHAR}, FilesScan, FilesBind<I>, FilesInit, FilesInitLocal);
	files.get_partition_data = FilesPartitionData;
	files.table_scan_progress = FilesProgress;
	files.cardinality = FilesCardinality;
	files.projection_pushdown = true;
	OrcidAddNamedParameter(files, "members", LogicalType::VARCHAR);
	OrcidAddNamedParameter(files, "max_records", LogicalType::BIGINT);
	OrcidAddNamedParameter(files, "max_member_bytes", LogicalType::BIGINT);
	OrcidAddNamedParameter(files, "json_root", LogicalType::VARCHAR);
	OrcidRegisterReader(loader, std::move(files), base, "ORCID");

	TableFunction xml(OrcidName(base + "_xml"), {LogicalType::VARCHAR}, nullptr, XmlBind<I>, nullptr, XmlInitLocal);
	xml.in_out_function = XmlInOut;
	loader.RegisterFunction(std::move(xml));

	TableFunctionSet json_set(OrcidName(base + "_json"));
	TableFunction json(OrcidName(base + "_json"), {LogicalType::VARCHAR}, nullptr, JsonBind<I>, nullptr,
	                   XmlInitLocal);
	json.in_out_function = XmlInOut;
	OrcidAddNamedParameter(json, "json_root", LogicalType::VARCHAR);
	json_set.AddFunction(json);
	// (json, json_root): the lateral form, see JsonRootColumn
	OrcidAddVarcharArgument(json);
	json_set.AddFunction(json);
	loader.RegisterFunction(std::move(json_set));
}

//! Must equal orcid::WriterNames().size(); checked at load.
static constexpr size_t WRITER_COUNT = 7;

template <size_t W>
static void RegisterWriter(ExtensionLoader &loader) {
	const string base = "orcid_" + orcid::WriterNames()[W];
	RegisterWriterScalar(loader, base + "_to_xml", WriterFun<W, false>);
	RegisterWriterScalar(loader, base + "_to_json", WriterFun<W, true>);
}

template <size_t... Ws>
static void RegisterWriters(ExtensionLoader &loader, std::index_sequence<Ws...>) {
	(RegisterWriter<Ws>(loader), ...);
}

template <size_t... Is>
static void RegisterSections(ExtensionLoader &loader, std::index_sequence<Is...>) {
	(RegisterSection<Is>(loader), ...);
}

static void LoadInternal(ExtensionLoader &loader) {
	if (orcid::Sections().size() != SECTION_COUNT) {
		throw InternalException("orcid: SECTION_COUNT (%llu) does not match the section tables (%llu)",
		                        static_cast<uint64_t>(SECTION_COUNT),
		                        static_cast<uint64_t>(orcid::Sections().size()));
	}
	if (orcid::WriterNames().size() != WRITER_COUNT) {
		throw InternalException("orcid: WRITER_COUNT (%llu) does not match the writer tables (%llu)",
		                        static_cast<uint64_t>(WRITER_COUNT),
		                        static_cast<uint64_t>(orcid::WriterNames().size()));
	}

	// Neither scalar can raise: invalid input yields false / NULL.
	OrcidRegisterScalar(loader,
	                    ScalarFunction("orcid_valid", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, OrcidValidFun),
	                    false);
	OrcidRegisterScalar(
	    loader, ScalarFunction("orcid_normalize", {LogicalType::VARCHAR}, LogicalType::VARCHAR, OrcidNormalizeFun),
	    false);

	RegisterSections(loader, std::make_index_sequence<SECTION_COUNT> {});

	TableFunction changes("orcid_changes", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ChangesFilesScan,
	                      ChangesFilesBind, ChangesFilesInit);
	OrcidAddNamedParameter(changes, "json_root", LogicalType::VARCHAR);
	loader.RegisterFunction(std::move(changes));
	TableFunction changes_xml("orcid_changes_xml", {LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr,
	                          ChangesXmlBind, nullptr, ChangesXmlInitLocal);
	changes_xml.in_out_function = ChangesXmlInOut;
	loader.RegisterFunction(std::move(changes_xml));
	TableFunctionSet changes_json_set("orcid_changes_json");
	TableFunction changes_json("orcid_changes_json", {LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr,
	                           ChangesJsonBind, nullptr, ChangesXmlInitLocal);
	changes_json.in_out_function = ChangesXmlInOut;
	OrcidAddNamedParameter(changes_json, "json_root", LogicalType::VARCHAR);
	changes_json_set.AddFunction(changes_json);
	OrcidAddVarcharArgument(changes_json); // (old, new, json_root), see JsonRootColumn
	changes_json_set.AddFunction(changes_json);
	loader.RegisterFunction(std::move(changes_json_set));

	OrcidAddBooleanSetting(loader, CROSSWALK_SETTING,
	                       "orcid writers: emit exactly the XML / JSON bytes of yorc-rs's document writers (the "
	                       "crosswalk format shared with yorc-zig) instead of yorc-duckdb's own layout; a writer's "
	                       "trailing BOOLEAN argument wins over it",
	                       false);
	RegisterWriters(loader, std::make_index_sequence<WRITER_COUNT> {});
	RegisterWriterScalar(loader, "orcid_bulk_works_to_xml", BulkWorksFun<false>);
	RegisterWriterScalar(loader, "orcid_bulk_works_to_json", BulkWorksFun<true>);

	TableFunction documents("orcid_documents", {LogicalType::VARCHAR}, DocumentsScan, DocumentsBind, DocumentsInit);
	OrcidAddNamedParameter(documents, "members", LogicalType::VARCHAR);
	OrcidAddNamedParameter(documents, "max_records", LogicalType::BIGINT);
	OrcidAddNamedParameter(documents, "max_member_bytes", LogicalType::BIGINT);
	documents.table_scan_progress = DocumentsProgress;
	loader.RegisterFunction(std::move(documents));

	TableFunctionSet samples("orcid_dump_sample");
	// LogicalTypeId, not the static constexpr LogicalType members: copying those odr-uses them (link error, e85ccef)
	for (auto seed_id : {LogicalTypeId::BIGINT, LogicalTypeId::UBIGINT}) {
		const LogicalType seed_type(seed_id);
		TableFunction sample("orcid_dump_sample", {LogicalType::VARCHAR, LogicalType::BIGINT, seed_type},
		                     DumpSampleScan, DumpSampleBind, DumpSampleInit);
		sample.projection_pushdown = true;
		OrcidAddNamedParameter(sample, "max_member_bytes", LogicalType::BIGINT);
		samples.AddFunction(std::move(sample));
	}
	loader.RegisterFunction(std::move(samples));
}

void YorcExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string YorcExtension::Name() {
	return "yorc";
}

std::string YorcExtension::Version() const {
#ifdef EXT_VERSION_YORC
	return EXT_VERSION_YORC;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(yorc, loader) {
	duckdb::LoadInternal(loader);
}
}
