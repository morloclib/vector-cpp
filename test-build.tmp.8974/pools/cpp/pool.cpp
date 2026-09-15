#include <string>
#include <iostream>
#include <sstream>
#include <functional>
#include <tuple>
#include <vector>
#include <algorithm>
#include <iterator>
#include <climits>
#include <stdexcept>
#include <fstream>
#include <system_error>
#include <unordered_map>
#include <sys/stat.h>
#include <sys/mman.h>
#include <csignal>
#ifdef __linux__
#include <sys/prctl.h>
#endif

// needed for foreign interface
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unistd.h>

#include <limits>
#include <utility>
#include <chrono>

char* g_tmpdir;

uint8_t* foreign_call(const char* socket_filename, size_t mid, ...) __attribute__((sentinel));
uint8_t* foreign_call_v(const char* socket_filename, size_t mid, const uint8_t** args_array, size_t nargs);

// AUTO include statements start
#include "/opt/morloc-state/src/morloc/plane/default/root-cpp/core.hpp"
#include "/opt/morloc-state/src/morloc/plane/default/vector-cpp/vector.hpp"
// AUTO include statements end

// Proper linking of cppmorloc requires it be included AFTER the custom modules
#include "mlc_arrow.hpp"
#include "cppmorloc.hpp"

// Defines mlc::Unit, which the generator emits for do-blocks whose final
// expression is void-returning (e.g. @save/@savej/@savem). Needs to follow
// the user-source includes so foreign code can use mlc::Unit too.
#include "mlccpptypes/prelude.hpp"

// Defined here (before PROPAGATE_ERROR and every throw site) so it is a
// COMPLETE type wherever it is thrown -- throwing an incomplete type is
// ill-formed C++ ([except.throw]/2); clang rejects it (g++ was lenient).
// MorlocException is the user-catchable class (@catch intercepts). Compiler
// / invariant bugs go through _mlc_internal_abort (below), which bypasses
// @catch. It carries a @throw / catchable-intrinsic-failure message across
// the pool call stack; also used by _mlc_read/_mlc_load/_mlc_throw below.
class MorlocException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define PROPAGATE_ERROR(errmsg) \
    if(errmsg != NULL) { \
      char errmsg_buffer[MAX_ERRMSG_SIZE] = { 0 }; \
      snprintf(errmsg_buffer, MAX_ERRMSG_SIZE, "Error C++ pool (%s:%d in %s):\n%s" , __FILE__, __LINE__, __func__, errmsg); \
      free(errmsg); \
      throw MorlocException(errmsg_buffer); \
    }

// A failure of the machinery that carries values between pools: IPC, packet
// construction and decode, shared-memory resolution. None of these are
// attributable to user data, paths or foreign-function behavior, and none
// leave the pool able to continue, so they must not be interceptable by
// @try. Deliberately NOT a MorlocException subclass, for the same reason
// MorlocPipeClosed is not: the broad catch in _mlc_catch must let it pass.
//
// The top-level dispatch turns this into an abort rather than a fail packet.
// A caller in another pool then sees its socket read fail, which is itself an
// infrastructure failure there, so fatality propagates across pools without
// the fail packet needing to carry a classification.
class MorlocInfraError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define PROPAGATE_INFRA_ERROR(errmsg) \
    if(errmsg != NULL) { \
      char errmsg_buffer[MAX_ERRMSG_SIZE] = { 0 }; \
      snprintf(errmsg_buffer, MAX_ERRMSG_SIZE, "Error C++ pool (%s:%d in %s):\n%s" , __FILE__, __LINE__, __func__, errmsg); \
      free(errmsg); \
      throw MorlocInfraError(errmsg_buffer); \
    }

// Terminal helper for morloc-compiler / runtime invariant violations
// (unreachable branches, contract violations from libmorloc, arity
// mismatches emitted by codegen). Prints a diagnostic to stderr, then
// aborts the pool process. The nexus sees the pool crash and treats
// it as a fatal error -- @catch never gets a chance to intercept.
// Use this only for genuine bugs: any error attributable to user data,
// paths, ascriptions, or foreign-function behavior must go through
// PROPAGATE_ERROR / MorlocException instead so @catch works.
[[noreturn]] static inline void _mlc_internal_abort(
    const char* file, int line, const char* func, const char* msg
) {
    std::fprintf(
        stderr,
        "morloc internal error (C++ pool, %s:%d in %s):\n  %s\n",
        file, line, func, msg
    );
    std::abort();
}

#define MLC_INTERNAL_ABORT(msg) _mlc_internal_abort(__FILE__, __LINE__, __func__, (msg))

#define PROPAGATE_FAIL_PACKET(errmsg) \
    if(errmsg != NULL){ \
        uint8_t* fail_packet_ = make_fail_packet(errmsg); \
        free(errmsg); \
        return fail_packet_; \
    }


// AUTO serialization statements start
static Schema* mlc_schema_table[34];
void _init_schemas() {
    static const char* _schema_strs[] = {
        "t2jj",
        "b",
        "j",
        "i1",
        "i2",
        "i4",
        "i8",
        "u1",
        "u2",
        "u4",
        "u8",
        "f4",
        "f8",
        "<std::vector<$1>>a:3j",
        "<std::vector<$1>>a:3i1",
        "<std::vector<$1>>a:3i2",
        "<std::vector<$1>>a:3i4",
        "<std::vector<$1>>a:3i8",
        "<std::vector<$1>>a:3u1",
        "<std::vector<$1>>a:3u2",
        "<std::vector<$1>>a:3u4",
        "<std::vector<$1>>a:3u8",
        "<std::vector<$1>>a:3f4",
        "<std::vector<$1>>a:3f8",
        "<std::vector<$1>>a:3b",
        "<std::vector<$1>>a:3s",
        "<std::vector<$1>>a:2t2js",
        "<std::vector<$1>>a:0j",
        "<std::vector<$1>>a:0f8",
        "<std::vector<$1>>a:4f8",
        "<std::vector<$1>>a:1f8",
        "<std::vector<$1>>a:6f8",
        "<std::vector<$1>>a:10j",
        "<std::vector<$1>>a:4j",
    };
    for (int i = 0; i < 34; i++)
        mlc_schema_table[i] = parse_schema_cpp(_schema_strs[i]);
}
// AUTO serialization statements end



std::string interweave_strings(const std::vector<std::string>& first, const std::vector<std::string>& second)
{
    // Validate sizes - errors here indicate a bug in the morloc compiler
    if (first.size() != second.size() + 1) {
        MLC_INTERNAL_ABORT(
            "interweave_strings: first list must have exactly 1 more element than second list"
        );
    }

    // Pre-calculate total size to avoid reallocations
    size_t total_size = 0;
    for (const auto& s : first) total_size += s.size();
    for (const auto& s : second) total_size += s.size();

    std::string result;
    result.reserve(total_size);

    // Interweave the strings
    for (size_t i = 0; i < second.size(); ++i) {
        result += first[i];
        result += second[i];
    }
    result += first.back();  // Append the final element from first list

    return result;
}

// Thread-local list of SHM pointers allocated by _put_value.
// Freed after foreign_call returns (args consumed) or at next dispatch start
// (result consumed by caller in the synchronous call that returned it).
struct ShmEntry { absptr_t ptr; };
// Releasing the entries is shared by the ordinary flush and by thread
// teardown, so it is written once and takes the container explicitly.
static void _shm_release_entries(std::vector<ShmEntry>& entries) {
    for (auto& e : entries) {
        char* err = NULL;
        // shfree decrements the refcount and zeros the block on final
        // ref-drop, so a separate metadata-zeroing pass is unnecessary.
        shfree(e.ptr, &err);
        if (err) { free(err); }
    }
    entries.clear();
}

// The tracker releases what it still holds when its thread ends. A worker
// is retired only after going idle for longer than the dispatch it would
// otherwise have been flushed by, so this is never earlier than the flush
// it stands in for -- it just happens on a thread that has no next
// dispatch to do it. Making the container itself own the teardown avoids
// depending on the destruction order of two thread-local objects.
struct ShmTracker : std::vector<ShmEntry> {
    ~ShmTracker() { _shm_release_entries(*this); }
};

thread_local ShmTracker _shm_tracker;

// Owns a block this pool materialized from a packet, releasing it unless
// ownership is handed elsewhere. Deserialization can throw, and a throwing
// dispatch is answered with a fail packet rather than ending the pool, so a
// block dropped on that path is lost once per bad request rather than once.
struct ShmOwned {
    absptr_t ptr;
    explicit ShmOwned(void* p) : ptr((absptr_t)p) {}
    ShmOwned(const ShmOwned&) = delete;
    ShmOwned& operator=(const ShmOwned&) = delete;
    ~ShmOwned() {
        if (ptr != nullptr) {
            char* err = NULL;
            shfree(ptr, &err);
            if (err) { free(err); }
        }
    }
};

static void _shm_tracker_flush() {
    _shm_release_entries(_shm_tracker);
    arrow_borrow_clear();
}

// Drop one tracker entry matching ptr (swap-with-last) and shfree the
// block. Used by _release_packet_shm to free
// a _put_value-tracked packet's SHM as soon as its codegen-determined
// scope ends, rather than waiting for the next dispatch flush.
static bool _shm_tracker_release_one(absptr_t ptr) {
    for (size_t i = 0; i < _shm_tracker.size(); i++) {
        if (_shm_tracker[i].ptr == ptr) {
            _shm_tracker[i] = _shm_tracker.back();
            _shm_tracker.pop_back();
            char* err = NULL;
            shfree(ptr, &err);
            if (err) { free(err); }
            return true;
        }
    }
    return false;
}

// Release the SHM ref owned by a _put_value-produced packet. The codegen
// inserts this call at the end of a serialize let's scope so the tracker
// entry is dropped as soon as the packet is no longer needed. No-op for
// inline (non-RPTR) packets, so callers can invoke unconditionally.
static void _release_packet_shm(const uint8_t* packet) {
    if (packet == nullptr) return;
    const morloc_packet_header_t* hdr = (const morloc_packet_header_t*)packet;
    if (hdr->command.data.source != PACKET_SOURCE_RPTR) return;
    size_t relptr = *(size_t*)(packet
        + sizeof(morloc_packet_header_t) + hdr->offset);
    char* resolve_err = NULL;
    void* voidstar = rel2abs(relptr, &resolve_err);
    if (resolve_err) { free(resolve_err); resolve_err = NULL; }
    if (voidstar) {
        _shm_tracker_release_one((absptr_t)voidstar);
    }
}

// Transforms a serialized value into a message ready for the socket
template <typename T>
uint8_t* _put_value(const T& value, Schema* schema) {
    if constexpr (std::is_same_v<T, mlc::ArrowTable>) {
        // Arrow export: move table data into SHM, build packet.
        // const_cast is safe here: the value is always a temporary from
        // a manifold call, never a truly const object.
        mlc::ArrowTable& tbl = const_cast<mlc::ArrowTable&>(value);
        relptr_t relptr = tbl.move_to_shm(schema);

        uint8_t* packet = make_arrow_data_packet(relptr, schema);
        if (!packet) { MLC_INTERNAL_ABORT("failed to create arrow data packet"); }

        char* err = nullptr;
        void* shm_ptr = rel2abs(relptr, &err);
        if (err) { free(err); }
        if (shm_ptr) { _shm_tracker.push_back({(absptr_t)shm_ptr}); }
        return packet;
    } else {
        // Arrow dispatch: schema marker `T` (MORLOC_TABLE) routes through
        // mlc::ArrowTable. The legacy `<arrow>` hint has been retired.
        if (schema->type == MORLOC_TABLE) {
            MLC_INTERNAL_ABORT("table schema but C++ type is not mlc::ArrowTable");
        }

        void* voidstar = nullptr;
        try {
            voidstar = to_voidstar(schema, value);
            relptr_t relptr = abs2rel_cpp(voidstar);

            char* errmsg = nullptr;
            uint8_t* packet = make_data_packet_auto(voidstar, relptr, schema, &errmsg);
            if (errmsg) {
                shfree_cpp(voidstar);
                PROPAGATE_INFRA_ERROR(errmsg);
            }

            const morloc_packet_header_t* hdr = (const morloc_packet_header_t*)packet;
            if (hdr->command.data.source == PACKET_SOURCE_RPTR) {
                // SHM referenced by packet -- track for deferred cleanup
                _shm_tracker.push_back({(absptr_t)voidstar});
            } else {
                // Data inlined in packet -- free SHM immediately. shfree
                // zeros the block on final ref-drop.
                char* free_err = NULL;
                shfree((absptr_t)voidstar, &free_err);
                if (free_err) { free(free_err); }
            }
            return packet;
        } catch (...) {
            if (voidstar) shfree_cpp(voidstar);
            throw;
        }
    }
}


// Use a key to retrieve a value
template <typename T>
T _get_value(const uint8_t* packet, Schema* schema){
    const morloc_packet_header_t* header = (const morloc_packet_header_t*)packet;
    uint8_t source = header->command.data.source;
    uint8_t format = header->command.data.format;

    if constexpr (std::is_same_v<T, mlc::ArrowTable>) {
        // Arrow import: packet -> validate -> arrow_from_shm -> ArrowTable
        if (format != PACKET_FORMAT_ARROW) {
            MLC_INTERNAL_ABORT("table-typed value did not arrive as an Arrow packet");
        }
        char* errmsg = nullptr;
        uint8_t* raw = get_morloc_data_packet_value(packet, schema, &errmsg);
        if (errmsg) { PROPAGATE_INFRA_ERROR(errmsg); }

        const arrow_shm_header_t* hdr = (const arrow_shm_header_t*)raw;
        char* verr = nullptr;
        if (arrow_validate(hdr, schema, &verr) != 0) {
            std::string msg(verr ? verr : "arrow table failed validation");
            free(verr);
            throw MorlocException(msg);
        }
        struct ArrowSchema as;
        struct ArrowArray aa;
        char* aerr = nullptr;
        arrow_from_shm(hdr, &as, &aa, &aerr);
        if (aerr) { PROPAGATE_INFRA_ERROR(aerr); }

        // A table that arrived by reference needs one taken on this pool's
        // behalf; a table materialized here is already this pool's own and
        // taking a second reference would leave it permanently held. Either
        // way the tracker releases exactly one at the next dispatch. A
        // refused acquire means nothing was taken, so nothing is tracked.
        bool arrow_owned = true;
        if (source == PACKET_SOURCE_RPTR) {
            char* ierr = nullptr;
            arrow_owned = shincref((absptr_t)raw, &ierr);
            if (ierr) { free(ierr); }
        }
        if (arrow_owned) {
            _shm_tracker.push_back({(absptr_t)raw});
            char* rerr = nullptr;
            relptr_t rel = abs2rel((absptr_t)raw, &rerr);
            if (rerr) { free(rerr); } else { arrow_borrow_register(raw, rel); }
        }

        return mlc::ArrowTable(std::move(as), std::move(aa));
    } else {
        if (format == PACKET_FORMAT_ARROW) {
            MLC_INTERNAL_ABORT("arrow data but C++ type is not mlc::ArrowTable");
        }

        // Stream ingest: FILE+DATA pointing at a stream-packet file. Nexus
        // emits this shape for `[a]` receivers; peek here to distinguish
        // stream from ordinary data-indirection. Each chunk's SHM is
        // shfree'd before the next @next, so peak SHM stays at one
        // sub-packet. Non-vector T instantiations throw at runtime as
        // defence-in-depth (nexus rejects such receivers upstream).
        if (source == PACKET_SOURCE_FILE && format == PACKET_FORMAT_DATA) {
            size_t payload_len = header->length;
            char path[PATH_MAX];
            if (payload_len >= sizeof(path)) {
                throw MorlocException("stream ingest: path exceeds PATH_MAX");
            }
            std::memcpy(path,
                        packet + sizeof(morloc_packet_header_t) + header->offset,
                        payload_len);
            path[payload_len] = '\0';
            if (file_is_stream_packet(path)) {
                if constexpr (is_std_vector<T>::value) {
                    char* open_err = nullptr;
                    int64_t handle = mlc_open(path, MLC_KIND_ISTREAM, &open_err);
                    if (open_err) { PROPAGATE_ERROR(open_err); }
                    // Preallocate via the stream's element_count (from
                    // StreamDiag) so vector::insert never reallocates.
                    char* len_err = nullptr;
                    int64_t total = mlc_ifile_length(handle, &len_err);
                    T result;
                    if (!len_err && total > 0) {
                        result.reserve((size_t)total);
                    }
                    if (len_err) free(len_err);
                    try {
                        while (true) {
                            char* nerr = nullptr;
                            void* chunk = mlc_next(handle, &nerr);
                            if (nerr) {
                                char* cerr = nullptr;
                                mlc_close(handle, &cerr);
                                if (cerr) free(cerr);
                                PROPAGATE_ERROR(nerr);
                            }
                            if (chunk == nullptr) break;
                            Array* arr = (Array*)chunk;
                            if (arr->size == 0) {
                                char* ferr = nullptr;
                                shfree((absptr_t)chunk, &ferr);
                                if (ferr) free(ferr);
                                break;
                            }
                            T* dummy = nullptr;
                            T chunk_vec = from_voidstar(schema, chunk, dummy);
                            char* ferr = nullptr;
                            shfree((absptr_t)chunk, &ferr);
                            if (ferr) free(ferr);
                            result.insert(result.end(),
                                          std::make_move_iterator(chunk_vec.begin()),
                                          std::make_move_iterator(chunk_vec.end()));
                        }
                    } catch (...) {
                        char* cerr = nullptr;
                        mlc_close(handle, &cerr);
                        if (cerr) free(cerr);
                        throw;
                    }
                    char* cerr = nullptr;
                    mlc_close(handle, &cerr);
                    if (cerr) free(cerr);
                    return result;
                } else {
                    MLC_INTERNAL_ABORT(
                        "stream indirection packet received for a "
                        "non-list receiver type; nexus should have rejected this upstream"
                    );
                }
            }
        }

        // Fast path: inline voidstar -- read directly from packet, no SHM
        // needed. A payload that is compressed or encrypted cannot be walked
        // where it lies, so it falls through to the general path, which
        // expands the body and re-enters. Testing for the plain values rather
        // than against the known transforms keeps a future one from being
        // read as raw bytes.
        if (source == PACKET_SOURCE_MESG && format == PACKET_FORMAT_VOIDSTAR
            && header->command.data.compression == PACKET_COMPRESSION_NONE
            && header->command.data.encryption == PACKET_ENCRYPTION_NONE) {
            const uint8_t* payload = packet + sizeof(morloc_packet_header_t) + header->offset;
            T* dummy = nullptr;
            return from_voidstar(schema, (const void*)payload, dummy, (const void*)payload);
        }

        // SHM paths (RPTR or MESG+MSGPACK): existing logic
        bool is_rptr = (source == PACKET_SOURCE_RPTR);

        char* errmsg = NULL;
        uint8_t* voidstar = get_morloc_data_packet_value(packet, schema, &errmsg);
        if(errmsg != NULL) {
            PROPAGATE_INFRA_ERROR(errmsg)
        }

        // A payload that did not arrive by reference was materialized into a
        // block of this pool's own -- one contiguous allocation covering the
        // whole value, so a single release covers it -- and nothing else will
        // ever free it. A payload that did arrive by reference belongs to its
        // sender.
        ShmOwned owned(is_rptr ? nullptr : (void*)voidstar);

        // For RPTR data, increment refcount so the owner's tracker flush
        // won't destroy data we may still need (e.g. forwarded packets).
        if (is_rptr) {
            char* incref_err = NULL;
            if (shincref((absptr_t)voidstar, &incref_err)) {
                _shm_tracker.push_back({(absptr_t)voidstar});
            }
            if (incref_err) { free(incref_err); }
        }

        T* dummy = nullptr;
        return from_voidstar(schema, (void*)voidstar, dummy);
    }
}


// Hash a value, returning a 16-char hex string
template <typename T>
std::string _mlc_hash(const T& value, Schema* schema) {
    void* voidstar = to_voidstar(schema, value);
    char* errmsg = NULL;
    char* hex = mlc_hash(voidstar, schema, &errmsg);
    shfree_cpp(voidstar);
    if (errmsg != NULL) {
        PROPAGATE_ERROR(errmsg)
    }
    std::string result(hex);
    free(hex);
    return result;
}

// Save a value to file in msgpack format. The `level` arg is accepted
// for ABI uniformity with mlc_save_voidstar and forwarded; the runtime
// ignores it for non-packet formats.
template <typename T>
void _mlc_save(const T& value, Schema* schema, int64_t level, const std::string& path) {
    void* voidstar = to_voidstar(schema, value);
    char* errmsg = NULL;
    mlc_save(voidstar, schema, (uint8_t)level, path.c_str(), &errmsg);
    shfree_cpp(voidstar);
    if (errmsg != NULL) {
        PROPAGATE_ERROR(errmsg)
    }
}

// Save a value to file in flat voidstar binary format. `level` is the
// zstd preset (0 = uncompressed, 1-9 = increasing ratio); the runtime
// stamps PACKET_COMPRESSION_ZSTD into the packet header when level > 0.
template <typename T>
void _mlc_save_voidstar(const T& value, Schema* schema, int64_t level, const std::string& path) {
    void* voidstar = to_voidstar(schema, value);
    char* errmsg = NULL;
    mlc_save_voidstar(voidstar, schema, (uint8_t)level, path.c_str(), &errmsg);
    shfree_cpp(voidstar);
    if (errmsg != NULL) {
        PROPAGATE_ERROR(errmsg)
    }
}

// Save a value to file in JSON format. `level` accepted for ABI uniformity.
template <typename T>
void _mlc_save_json(const T& value, Schema* schema, int64_t level, const std::string& path) {
    void* voidstar = to_voidstar(schema, value);
    char* errmsg = NULL;
    mlc_save_json(voidstar, schema, (uint8_t)level, path.c_str(), &errmsg);
    shfree_cpp(voidstar);
    if (errmsg != NULL) {
        PROPAGATE_ERROR(errmsg)
    }
}

// Serialize a value to a JSON string
template <typename T>
std::string _mlc_show(const T& value, Schema* schema) {
    void* voidstar = to_voidstar(schema, value);
    char* errmsg = NULL;
    char* json = mlc_show(voidstar, schema, &errmsg);
    shfree_cpp(voidstar);
    if (errmsg != NULL) {
        PROPAGATE_ERROR(errmsg)
    }
    std::string result(json);
    free(json);
    return result;
}

// (MorlocException is defined near the top of this file, before its first
// throw site -- see the note there.)

// MorlocPipeClosed signals that the downstream consumer closed @stdout /
// @stderr (a broken pipe). It is an <IO> condition, NOT a user-recoverable
// <Err>, so it is deliberately NOT a subclass of MorlocException: _mlc_catch
// lets it propagate rather than routing it into a fallback. The pool's
// top-level dispatch converts it to a distinguished fail packet (the nexus,
// which owns fd 1 and already saw the EPIPE, decides the exit status).
class MorlocPipeClosed : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Reserved return code from mlc_write / mlc_flush / mlc_close: the write
// hit EPIPE. The runtime returns this WITHOUT setting errmsg. Kept in
// sync with morloc_runtime_types::MLC_RESULT_PIPE_CLOSED (= 2).
static constexpr int MLC_RESULT_PIPE_CLOSED = 2;

// Throw the distinguished broken-pipe exception when a stdio C-ABI call
// returns the reserved pipe-closed code. Shared by @write/@flush/@close.
static inline void _mlc_throw_if_pipe_closed(int rc) {
    if (rc == MLC_RESULT_PIPE_CLOSED) {
        throw MorlocPipeClosed("@stdout: downstream pipe closed");
    }
}

// Deserialize a JSON string to a typed value.
// @read :: Str -> <Err> a. Parse failure throws MorlocException so
// _mlc_catch can intercept.
template <typename T>
T _mlc_read(Schema* schema, const std::string& json_str) {
    char* errmsg = NULL;
    void* voidstar = mlc_read(json_str.c_str(), schema, &errmsg);
    if (errmsg != NULL) {
        std::string msg(errmsg);
        free(errmsg);
        throw MorlocException(std::string("@read: ") + msg);
    }
    if (voidstar == NULL) {
        throw MorlocException("@read: parse failed on '" + json_str + "'");
    }
    T* dummy = nullptr;
    T result = from_voidstar(schema, voidstar, dummy);
    shfree_cpp(voidstar);
    return result;
}

// Load a value from file, auto-detecting format.
// @load :: Str -> <IO, Err> a. Missing file / decode failure throws
// MorlocException so _mlc_catch can intercept.
template <typename T>
T _mlc_load(Schema* schema, const std::string& path) {
    char* errmsg = NULL;
    void* voidstar = mlc_load(path.c_str(), schema, &errmsg);
    if (errmsg != NULL) {
        std::string msg(errmsg);
        free(errmsg);
        throw MorlocException(std::string("@load: ") + msg);
    }
    if (voidstar == NULL) {
        throw MorlocException("@load: failed to load '" + path + "'");
    }
    T* dummy = nullptr;
    T result = from_voidstar(schema, voidstar, dummy);
    shfree_cpp(voidstar);
    return result;
}

// ── Stream-handle wrappers ───────────────────────────────────────────────
// @open: returns a 64-bit handle (kind = MLC_KIND_{IFILE|ISTREAM|OSTREAM}).
inline int64_t _mlc_open(const std::string& path, uint8_t kind) {
    char* errmsg = NULL;
    int64_t handle = mlc_open(path.c_str(), kind, &errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    return handle;
}
// @close: returns void; the runtime bumps generation and frees the slot.
inline void _mlc_close(int64_t handle) {
    char* errmsg = NULL;
    int rc = mlc_close(handle, &errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    _mlc_throw_if_pipe_closed(rc);
}

// _mlc_throw returns a universal-conversion helper so that
// `<cond> ? _mlc_throw(msg) : someInt` type-checks in either branch of a
// conditional (both arms of morloc's `?`/`:` lower to lambdas of the same
// return type). The constructor throws, so control never reaches the
// operator T() bodies; std::terminate is just a defence in depth.
struct _MlcThrowHelper {
    template<typename T> operator T() const { std::terminate(); }
};
inline _MlcThrowHelper _mlc_throw(const std::string& msg) {
    throw MorlocException(msg);
}
// @catch: run `fallible()`; on a recoverable error, run `fallback()` and
// return its result. Template deduction picks the return type from the
// fallible thunk. The fallback must return the same type.
//
// @try body: run the thunk and convert the outcome to data. `ok` wraps the
// value, `err` the message; codegen supplies both because only it knows how
// this Try is represented in C++.
//
// MorlocPipeClosed and MorlocInfraError are re-thrown ahead of the broad
// catch. A broken pipe is an <IO> condition and an infrastructure failure is
// not attributable to user code, so neither is a user-recoverable error and
// both must escape rather than being turned into an Err arm. All other
// std::exceptions (@throw, foreign helpers that raise) become Err.
// The return type is deduced from `err` rather than `ok`: a Unit-returning
// intrinsic's body is `void`, so `ok(body())` is ill-formed as a deduction
// context even though the void branch never evaluates it.
template<typename FL, typename OK, typename ERR>
auto _mlc_try(FL&& body, OK&& ok, ERR&& err) -> decltype(err(std::string())) {
    try {
        if constexpr (std::is_void_v<decltype(body())>) {
            body();
            return ok(mlc::Unit{});
        } else {
            return ok(body());
        }
    }
    catch (const MorlocPipeClosed&) { throw; }
    catch (const MorlocInfraError&) { throw; }
    catch (const std::exception& e) { return err(std::string(e.what())); }
}
// @fschema: read a file's element schema string without opening it.
inline std::string _mlc_fschema(const std::string& path) {
    char* errmsg = NULL;
    char* s = mlc_fschema(path.c_str(), &errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    std::string out(s ? s : "");
    if (s) free(s);
    return out;
}
// @flen: total element count of an IFile (`length f`). Cheap; reads
// from the cached StreamDiag at open time.
inline int64_t _mlc_ifile_length(int64_t handle) {
    char* errmsg = NULL;
    int64_t n = mlc_ifile_length(handle, &errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    return n;
}

// @ifile_walk: unified IFile pattern walker. The `path` encodes the
// walk-step chain (".[]", ".[:]", ".1.foo", etc.); `args` carries the
// runtime bracket bounds (Python-style optional ints; absent slots
// take the default). T is the materialized result type the wrapper
// converts the returned voidstar into.
template <typename T>
T _mlc_ifile_walk(
    Schema* schema, int64_t handle,
    const std::string& path,
    std::initializer_list<std::optional<int64_t>> args
) {
    // Pack the optional<int64_t> args into the C-ABI struct array. The
    // struct is fixed-size (16 bytes) so a contiguous vector is the
    // natural marshal target. Empty initializer -> nullptr / n_args=0.
    std::vector<mlc_ifile_walk_arg> packed;
    packed.reserve(args.size());
    for (const auto& a : args) {
        mlc_ifile_walk_arg w{};
        if (a.has_value()) { w.has = 1; w.value = *a; }
        packed.push_back(w);
    }
    char* errmsg = NULL;
    void* voidstar = mlc_ifile_walk(
        handle, path.c_str(),
        packed.empty() ? nullptr : packed.data(),
        (uint64_t)packed.size(),
        &errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    T* dummy = nullptr;
    T result = from_voidstar(schema, voidstar, dummy);
    shfree_cpp(voidstar);
    return result;
}

// @next: materialise the IStream's current sub-packet as `[a]` and
// advance the cursor. Empty list at EOF.
template <typename T>
T _mlc_next(Schema* schema, int64_t handle) {
    char* errmsg = NULL;
    void* voidstar = mlc_next(handle, &errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    T* dummy = nullptr;
    T result = from_voidstar(schema, voidstar, dummy);
    shfree_cpp(voidstar);
    return result;
}

// @streamLayout: per-sub-packet layout of an IFile as `[(U64,U64,U64)]`.
// Each triple is (element_offset, element_count, uncompressed_size). T is
// the materialized result type (e.g. std::vector<std::tuple<...>>).
template <typename T>
T _mlc_stream_layout(Schema* schema, int64_t handle) {
    char* errmsg = NULL;
    void* voidstar = mlc_stream_layout(handle, &errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    T* dummy = nullptr;
    T result = from_voidstar(schema, voidstar, dummy);
    shfree_cpp(voidstar);
    return result;
}

// @stream: derive an IStream handle from an open IFile handle.
inline int64_t _mlc_stream(int64_t ifile_handle) {
    char* errmsg = NULL;
    int64_t h = mlc_stream(ifile_handle, &errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    return h;
}

// @open path :: <IO> (OStream T) -- typed open. The schema string for T
// is baked into the codegen and passed alongside the path. We serialise
// the parsed Schema back to its canonical string via libmorloc's
// `schema_to_string` helper.
inline int64_t _mlc_open_ostream(Schema* schema, const std::string& path) {
    char* errmsg = NULL;
    char* s = schema_to_string(schema);
    if (s == NULL) {
        MLC_INTERNAL_ABORT("_mlc_open_ostream: schema_to_string returned NULL (invalid compile-time schema table entry)");
    }
    int64_t h = mlc_open_ostream(s, path.c_str(), &errmsg);
    free(s);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    return h;
}

// Typed IStream open. The element schema declares the opener's type so
// the runtime can route the /dev/stdin sentinel through the nexus stdin
// channel (guarding the incoming stream); for a real file it is read off
// disk.
inline int64_t _mlc_open_istream(Schema* schema, const std::string& path) {
    char* errmsg = NULL;
    char* s = schema_to_string(schema);
    if (s == NULL) {
        MLC_INTERNAL_ABORT("_mlc_open_istream: schema_to_string returned NULL (invalid compile-time schema table entry)");
    }
    int64_t h = mlc_open_istream(s, path.c_str(), &errmsg);
    free(s);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    return h;
}

// @stdin / @stdout / @stderr: nullary intrinsics. Element schema is
// rendered from the parsed Schema and passed to the runtime; the nexus
// owns fd 0/1/2 and services @next / @write over the RPC socket.
inline int64_t _mlc_open_stdin(Schema* schema) {
    char* errmsg = NULL;
    char* s = schema_to_string(schema);
    if (s == NULL) MLC_INTERNAL_ABORT("_mlc_open_stdin: schema_to_string returned NULL");
    int64_t h = mlc_open_stdin(s, &errmsg);
    free(s);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    return h;
}

inline int64_t _mlc_open_stdout(Schema* schema) {
    char* errmsg = NULL;
    char* s = schema_to_string(schema);
    if (s == NULL) MLC_INTERNAL_ABORT("_mlc_open_stdout: schema_to_string returned NULL");
    int64_t h = mlc_open_stdout(s, &errmsg);
    free(s);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    return h;
}

inline int64_t _mlc_open_stderr(Schema* schema) {
    char* errmsg = NULL;
    char* s = schema_to_string(schema);
    if (s == NULL) MLC_INTERNAL_ABORT("_mlc_open_stderr: schema_to_string returned NULL");
    int64_t h = mlc_open_stderr(s, &errmsg);
    free(s);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    return h;
}

// @write: serialize T to voidstar and emit one sub-packet.
template <typename T>
inline void _mlc_write(Schema* schema, int64_t level, const T& value, int64_t handle) {
    char* errmsg = NULL;
    // Stage value into a SHM block via to_voidstar so the runtime can
    // read it as a contiguous voidstar payload. The eval_arena tracks
    // the allocation; we shfree explicitly after the runtime is done.
    size_t bytes = get_shm_size(schema, value);
    void* voidstar = shmalloc_cpp(bytes);
    void* cursor = (uint8_t*)voidstar + schema->width;
    to_voidstar(voidstar, &cursor, schema, value);
    int rc = mlc_write(static_cast<uint8_t>(level), handle, voidstar, &errmsg);
    shfree_cpp(voidstar);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    _mlc_throw_if_pipe_closed(rc);
    if (rc != 0) {
        MLC_INTERNAL_ABORT("mlc_write returned non-zero without setting errmsg (libmorloc contract violation)");
    }
}

// @append: open existing file for append; schema must match.
inline int64_t _mlc_append(Schema* schema, const std::string& path) {
    char* errmsg = NULL;
    char* s = schema_to_string(schema);
    if (s == NULL) {
        MLC_INTERNAL_ABORT("_mlc_append: schema_to_string returned NULL (invalid compile-time schema table entry)");
    }
    int64_t h = mlc_append(s, path.c_str(), &errmsg);
    free(s);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    return h;
}

// @concat: byte-level concat of stream files via sendfile.
inline void _mlc_concat(const std::vector<std::string>& paths, const std::string& dest) {
    char* errmsg = NULL;
    std::vector<const char*> raw_paths;
    raw_paths.reserve(paths.size());
    for (const auto& p : paths) {
        raw_paths.push_back(p.c_str());
    }
    int rc = mlc_concat(
        raw_paths.empty() ? nullptr : raw_paths.data(),
        raw_paths.size(),
        dest.c_str(),
        &errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    if (rc != 0) {
        MLC_INTERNAL_ABORT("mlc_concat returned non-zero without setting errmsg (libmorloc contract violation)");
    }
}

// @flush: force buffered writes to be emitted as a sub-packet now.
inline void _mlc_flush(int64_t handle) {
    char* errmsg = NULL;
    int rc = mlc_flush(handle, &errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    _mlc_throw_if_pipe_closed(rc);
    if (rc != 0) {
        MLC_INTERNAL_ABORT("mlc_flush returned non-zero without setting errmsg (libmorloc contract violation)");
    }
}

// @tell: elements written to @stdout so far (its element_count). Never fails.
inline uint64_t _mlc_tell() {
    char* errmsg = NULL;
    uint64_t n = mlc_tell(&errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    return n;
}

// @tmpfile: create a fresh temp file (removed when the pool call ends) and
// return its path. Used by the whole-list with:/render: gather.
inline std::string _mlc_tmpfile() {
    char* errmsg = NULL;
    char* path = mlc_tmpfile(&errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
    std::string result(path);
    free(path);
    return result;
}

// @close on a Str path: unlink a registered temp file. Errors if the path
// was not created by _mlc_tmpfile in this call.
inline void _mlc_unlink_tmp(const std::string& path) {
    char* errmsg = NULL;
    mlc_unlink_tmp(path.c_str(), &errmsg);
    if (errmsg != NULL) { PROPAGATE_ERROR(errmsg) }
}

// Array-based foreign call: send a local-call packet carrying a runtime-sized
// list of argument packets to the pool listening on `socket_filename`. Used by
// the variadic `foreign_call` below and by defunctionalized-closure reflection,
// where the captured-argument count is only known at runtime. The caller owns
// `args_array`; on an error this throws (via PROPAGATE_ERROR), so the caller's
// container unwinds normally.
uint8_t* foreign_call_v(const char* socket_filename, size_t mid, const uint8_t** args_array, size_t nargs) {
    char* errmsg = NULL;

    char socket_path[128];
    snprintf(socket_path, sizeof(socket_path), "%s/%s", g_tmpdir, socket_filename);

    uint8_t* packet = make_morloc_local_call_packet((uint32_t)mid, args_array, nargs, &errmsg);
    PROPAGATE_INFRA_ERROR(errmsg)

    pool_mark_busy();
    uint8_t* result = send_and_receive_over_socket(socket_path, packet, &errmsg);
    pool_mark_idle();

    free(packet);
    PROPAGATE_INFRA_ERROR(errmsg)

    {
        char* fail_check_err = NULL;
        char* fail_msg = get_morloc_data_packet_error_message(
            (const uint8_t*)result, &fail_check_err);
        if (fail_check_err != NULL) { free(fail_check_err); }
        if (fail_msg != NULL) {
            std::string msg(fail_msg);
            free(fail_msg);
            free(result);
            throw MorlocException(msg);
        }
    }

    // Incref the result's SHM so the callee's tracker flush won't destroy
    // data we may still need (mirrors the variadic foreign_call).
    {
        const morloc_packet_header_t* res_header = (const morloc_packet_header_t*)result;
        if (res_header->command.data.source == PACKET_SOURCE_RPTR) {
            size_t relptr = *(size_t*)(result + res_header->offset + sizeof(morloc_packet_header_t));
            char* resolve_err = NULL;
            void* res_voidstar = rel2abs(relptr, &resolve_err);
            if (resolve_err) { free(resolve_err); resolve_err = NULL; }
            if (res_voidstar) {
                // The callee took a reference before sending; it is ours
                // now. Inherit it rather than adding another.
                _shm_tracker.push_back({(absptr_t)res_voidstar});
            }
        }
    }

    return result;
}

// Variadic wrapper over foreign_call_v: gather a compile-time NULL-terminated
// argument list into an array and delegate. The array is freed on both the
// normal-return and the throw path so no ownership leaks across foreign_call_v.
uint8_t* foreign_call(const char* socket_filename, size_t mid, ...) {
    va_list args;
    size_t nargs = 0;

    // Count arguments (must be NULL-terminated)
    va_start(args, mid);
    while (va_arg(args, uint8_t*) != NULL) nargs++;
    va_end(args);

    // Allocate and populate args array
    const uint8_t** args_array = (const uint8_t**)malloc((nargs + 1) * sizeof(uint8_t*));
    if (!args_array) MLC_INTERNAL_ABORT("malloc failed in foreign_call (OOM)");

    va_start(args, mid);
    for (size_t i = 0; i < nargs; i++) {
        args_array[i] = va_arg(args, uint8_t*);
    }
    args_array[nargs] = NULL;  // Sentinel
    va_end(args);

    uint8_t* result;
    try {
        result = foreign_call_v(socket_filename, mid, args_array, nargs);
    } catch (...) {
        free(args_array);
        throw;
    }
    free(args_array);
    return result;
}


// Defunctionalized-closure support (C++ producer side). A morloc function value
// is a std::function that actually holds a MorlocClosure functor carrying the
// manifold id and a reifier for its captured values. Applying it forwards to
// the wrapped native call (intra-language cost unchanged); reifying it (when it
// crosses a language boundary) recovers (home_language, mid, captured_packets)
// so the far side can call back via foreign_call on the mid. The value is stored
// through std::function so cppTypeOf is unchanged -- recovery is via target<>.
// A closure value has the concrete signature 'R(A...)'. It is callable
// (operator() forwards to the wrapped native bind -- intra-language cost is one
// extra indirection, no different from a std::function) so it works both when
// stored in a std::function<R(A...)> and when a template HOF deduces its type
// directly; and it carries the manifold id + captured-value reifier so that,
// when it crosses a boundary, reify can recover (home_language, mid, captured).
// A closure reflected from another pool is a MorlocClosure too, whose
// `home` names that pool: reifying it hands back the origin it came with.
template <class Sig> struct MorlocClosure;
template <class R, class... A>
struct MorlocClosure<R(A...)> {
    std::function<R(A...)> fn;
    int64_t mid;
    std::function<std::vector<std::vector<uint8_t>>()> reify_captured;
    std::string home = "cpp";
    R operator()(A... args) const { return fn(args...); }
};

// Serialize one captured native value into a SELF-CONTAINED packet for the
// closure wire form. The inline threshold is forced to max so the value is
// embedded in the packet (PACKET_SOURCE_MESG) rather than left as a shared-
// memory relptr (PACKET_SOURCE_RPTR): a relptr packet dangles once the
// producing manifold's SHM is reclaimed, which happens before the crossed
// closure is applied back, silently losing the captured data. The returned
// byte vector owns its bytes and is portable across pools (and --no-shm).
template <typename T>
std::vector<uint8_t> _mlc_reify_capture(const T& value, Schema* schema) {
    uint64_t saved = morloc_get_inline_threshold();
    morloc_set_inline_threshold(INT64_MAX);
    uint8_t* packet;
    try {
        packet = _put_value(value, schema);
    } catch (...) {
        morloc_set_inline_threshold((int64_t)saved);
        throw;
    }
    morloc_set_inline_threshold((int64_t)saved);
    const morloc_packet_header_t* h = (const morloc_packet_header_t*)packet;
    size_t n = sizeof(morloc_packet_header_t) + h->offset + h->length;
    return std::vector<uint8_t>(packet, packet + n);
}

// Reify a crossing closure into its wire tuple (home_language, mid, captured).
template <class R, class... A>
std::tuple<std::string, int64_t, std::vector<std::vector<uint8_t>>>
_mlc_reify(const std::function<R(A...)>& f) {
    const auto* clo = f.template target<MorlocClosure<R(A...)>>();
    if (clo == nullptr) {
        throw MorlocException("cannot reify a non-morloc C++ closure");
    }
    return std::make_tuple(clo->home, clo->mid, clo->reify_captured());
}



// Debug-trace runtime hooks (defined in libmorloc). Declared here,
// above the AUTO splice points, so codegen-emitted manifold bodies
// see the prototype for morloc_debug_record_frame. drain/flush are
// only called from the in-template handlers below, but the whole
// trio is grouped here for cohesion.
extern "C" char* morloc_debug_drain_frames();
extern "C" void morloc_debug_flush_dispatch();
extern "C" void morloc_debug_record_frame(
    uint32_t midx,
    const char* name,
    const char* srcloc,
    const char* lang,
    const uint8_t** packets,
    const char** schemas,
    size_t n);



// AUTO signatures statements start
uint8_t* m1();
uint8_t* m3397();
uint8_t* m3490();
uint8_t* m3508();
uint8_t* m3526();
uint8_t* m3544();
uint8_t* m3560();
uint8_t* m3576();
uint8_t* m3592();
uint8_t* m3608();
uint8_t* m3624();
uint8_t* m3636();
uint8_t* m3652();
uint8_t* m3668();
uint8_t* m3690();
uint8_t* m3869();
uint8_t* m3885();
uint8_t* m3901();
uint8_t* m3917();
uint8_t* m3933();
uint8_t* m3949();
uint8_t* m3965();
uint8_t* m3981();
uint8_t* m3997();
uint8_t* m4013();
uint8_t* m4029();
uint8_t* m4045();
uint8_t* m4059();
uint8_t* m4161();
uint8_t* m4172();
uint8_t* m4183();
uint8_t* m4194();
uint8_t* m4205();
uint8_t* m4216();
uint8_t* m4227();
uint8_t* m4238();
uint8_t* m4249();
uint8_t* m4260();
uint8_t* m4271();
uint8_t* m4282();
uint8_t* m4293();
uint8_t* m4304();
uint8_t* m4315();
uint8_t* m4326();
uint8_t* m4337();
uint8_t* m4348();
uint8_t* m4359();
uint8_t* m4370();
uint8_t* m4381();
uint8_t* m4392();
uint8_t* m4403();
uint8_t* m4414();
uint8_t* m4425();
uint8_t* m4432();
uint8_t* m4443();
uint8_t* m4450();
uint8_t* m4461();
uint8_t* m4471();
uint8_t* m4483();
uint8_t* m4491();
uint8_t* m4694();
uint8_t* m4698();
uint8_t* m4706();
uint8_t* m4710();
uint8_t* m4719();
uint8_t* m4723();
uint8_t* m4781();
uint8_t* m4784();
uint8_t* m4792();
uint8_t* m4795();
uint8_t* m4804();
uint8_t* m4807();
uint8_t* m4816();
uint8_t* m4819();
uint8_t* m4890();
uint8_t* m4893();
uint8_t* m4901();
uint8_t* m4904();
uint8_t* m4913();
uint8_t* m4916();
uint8_t* m4925();
uint8_t* m4928();
uint8_t* m4989();
uint8_t* m5002();
uint8_t* m5014(const uint8_t* s57);
uint8_t* m5026(const uint8_t* s57);
uint8_t* m5038(const uint8_t* s57);
uint8_t* m5050(const uint8_t* s57);
uint8_t* m5065(const uint8_t* s57);
uint8_t* m5079(const uint8_t* s57);
uint8_t* m5093(const uint8_t* s57);
uint8_t* m5107(const uint8_t* s57);
uint8_t* m5121(const uint8_t* s57);
uint8_t* m5137(const uint8_t* s57);
uint8_t* m5154(const uint8_t* s57);
uint8_t* m5169(const uint8_t* s57);
uint8_t* m5183(const uint8_t* s57);
uint8_t* m5197(const uint8_t* s57);
uint8_t* m5212(const uint8_t* s57);
uint8_t* m5231(const uint8_t* s57);
uint8_t* m5246(const uint8_t* s57);
uint8_t* m5260(const uint8_t* s57);
uint8_t* m5279(const uint8_t* s59);
uint8_t* m5297(const uint8_t* s57);
uint8_t* m5316(const uint8_t* s57);
uint8_t* m5333(const uint8_t* s57);
uint8_t* m5350(const uint8_t* s57);
uint8_t* m5367(const uint8_t* s57);
uint8_t* m5384(const uint8_t* s57);
uint8_t* m5401(const uint8_t* s57);
uint8_t* m5418(const uint8_t* s57);
uint8_t* m5433(const uint8_t* s57);
// AUTO signatures statements end



// AUTO manifolds statements start
std::tuple<int,int> m3449()
{
    try
    {
        uint8_t* s1 = foreign_call("pipe-py", 3449, NULL);
        return(_get_value<std::tuple<int,int>>(s1, mlc_schema_table[0]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3449, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3449, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3449, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3449, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)");
    }
}

uint8_t* m1()
{
    try
    {
        int n2 = std::get<0>(m3449());
        bool n3 = morloc_eq(0, n2);
        return(_put_value(n3, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at test [cpp] (mid=1, test.loc:1:25)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at test [cpp] (mid=1, test.loc:1:25)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at test [cpp] (mid=1, test.loc:1:25)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at test [cpp] (mid=1, test.loc:1:25)");
    }
}
uint8_t* m3397()
{
    try
    {
        std::tuple<int,int> n1 = std::make_tuple(0, 0);
        return(_put_value(n1, mlc_schema_table[0]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3397, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:72:17)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3397, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:72:17)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3397, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:72:17)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3397, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:72:17)");
    }
}
int m3716(const int& n0)
{
    try
    {
        return(n0);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3716, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3716, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3716, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3716, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3490()
{
    try
    {
        std::vector<int> n2 = {1,2,3};
        std::vector<int> n3 = morloc_vec_map( std::bind(m3716,
        std::placeholders::_1)
        , n2 );
        std::vector<int> n1 = {1,2,3};
        bool n4 = morloc_vec_eq(n1, n3);
        return(_put_value(n4, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3490, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:214:73)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3490, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:214:73)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3490, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:214:73)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3490, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:214:73)");
    }
}
int m3727(const int& n1)
{
    try
    {
        return(n1);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3727, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3727, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3727, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3727, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3508()
{
    try
    {
        std::vector<int> n3 = {1,2,4};
        std::vector<int> n4 = morloc_vec_map( std::bind(m3727,
        std::placeholders::_1)
        , n3 );
        std::vector<int> n2 = {1,2,3};
        bool n5 = morloc_vec_eq(n2, n4);
        return(_put_value(n5, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3508, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:215:73)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3508, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:215:73)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3508, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:215:73)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3508, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:215:73)");
    }
}
int64_t m3738(const int64_t& n2)
{
    try
    {
        return(n2);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3738, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3738, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3738, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3738, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3526()
{
    try
    {
        std::vector<int64_t> n4 = {static_cast<int64_t>(1)
        ,static_cast<int64_t>(2)
        ,static_cast<int64_t>(3)};
        std::vector<int64_t> n5 = morloc_vec_map( std::bind(m3738,
        std::placeholders::_1)
        , n4 );
        std::vector<int64_t> n3 = {static_cast<int64_t>(1)
        ,static_cast<int64_t>(2)
        ,static_cast<int64_t>(3)};
        bool n6 = morloc_vec_eq(n3, n5);
        return(_put_value(n6, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3526, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:216:69)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3526, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:216:69)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3526, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:216:69)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3526, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:216:69)");
    }
}
double m3749(const double& n3)
{
    try
    {
        return(n3);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3749, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3749, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3749, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3749, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3544()
{
    try
    {
        std::vector<double> n5 = {1.5,2.5};
        std::vector<double> n6 = morloc_vec_map( std::bind(m3749,
        std::placeholders::_1)
        , n5 );
        std::vector<double> n4 = {1.5,2.5};
        bool n7 = morloc_vec_eq(n4, n6);
        return(_put_value(n7, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3544, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:217:66)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3544, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:217:66)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3544, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:217:66)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3544, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:217:66)");
    }
}
double m3760(const double& n4)
{
    try
    {
        return(n4);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3760, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3760, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3760, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3760, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3560()
{
    try
    {
        std::vector<double> n6 = {1.5,2.5};
        std::vector<double> n7 = morloc_vec_map( std::bind(m3760,
        std::placeholders::_1)
        , n6 );
        std::vector<double> n5 = {1.5,2.5};
        bool n8 = morloc_vec_eq(n5, n7);
        return(_put_value(n8, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3560, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:218:73)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3560, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:218:73)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3560, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:218:73)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3560, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:218:73)");
    }
}
double m3771(const double& n5)
{
    try
    {
        return(n5);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3771, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3771, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3771, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3771, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3576()
{
    try
    {
        std::vector<double> n7 = {1.5,9.9};
        std::vector<double> n8 = morloc_vec_map( std::bind(m3771,
        std::placeholders::_1)
        , n7 );
        std::vector<double> n6 = {1.5,2.5};
        bool n9 = morloc_vec_eq(n6, n8);
        return(_put_value(n9, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3576, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:219:73)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3576, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:219:73)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3576, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:219:73)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3576, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:219:73)");
    }
}
bool m3782(const bool& n6)
{
    try
    {
        return(n6);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3782, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3782, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3782, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3782, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3592()
{
    try
    {
        std::vector<bool> n8 = {true,false};
        std::vector<bool> n9 = morloc_vec_map( std::bind(m3782,
        std::placeholders::_1)
        , n8 );
        std::vector<bool> n7 = {true,false};
        bool n10 = morloc_vec_eq(n7, n9);
        return(_put_value(n10, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3592, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:220:74)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3592, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:220:74)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3592, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:220:74)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3592, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:220:74)");
    }
}
bool m3793(const bool& n7)
{
    try
    {
        return(n7);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3793, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3793, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3793, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3793, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3608()
{
    try
    {
        std::vector<bool> n9 = {true,true};
        std::vector<bool> n10 = morloc_vec_map( std::bind(m3793,
        std::placeholders::_1)
        , n9 );
        std::vector<bool> n8 = {true,false};
        bool n11 = morloc_vec_eq(n8, n10);
        return(_put_value(n11, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3608, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:221:74)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3608, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:221:74)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3608, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:221:74)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3608, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:221:74)");
    }
}
int m3804(const int& n8)
{
    try
    {
        return(n8);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3804, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3804, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3804, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3804, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3624()
{
    try
    {
        std::vector<int> n10 = {};
        std::vector<int> n11 = morloc_vec_map( std::bind(m3804,
        std::placeholders::_1)
        , n10 );
        std::vector<int> n9 = {};
        bool n12 = morloc_vec_eq(n9, n11);
        return(_put_value(n12, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3624, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:222:73)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3624, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:222:73)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3624, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:222:73)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3624, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:222:73)");
    }
}
std::string m3815(const std::string& n9)
{
    try
    {
        return(n9);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3815, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3815, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3815, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3815, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3636()
{
    try
    {
        std::vector<std::string> n11 = {std::string("a", 1)
        ,std::string("b", 1)};
        std::vector<std::string> n12 = morloc_vec_map( std::bind(m3815,
        std::placeholders::_1)
        , n11 );
        std::vector<std::string> n10 = {std::string("a", 1)
        ,std::string("b", 1)};
        bool n13 = morloc_vec_eq(n10, n12);
        return(_put_value(n13, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3636, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:223:73)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3636, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:223:73)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3636, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:223:73)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3636, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:223:73)");
    }
}
std::string m3826(const std::string& n10)
{
    try
    {
        return(n10);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3826, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3826, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3826, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3826, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3652()
{
    try
    {
        std::vector<std::string> n12 = {std::string("a", 1)
        ,std::string("c", 1)};
        std::vector<std::string> n13 = morloc_vec_map( std::bind(m3826,
        std::placeholders::_1)
        , n12 );
        std::vector<std::string> n11 = {std::string("a", 1)
        ,std::string("b", 1)};
        bool n14 = morloc_vec_eq(n11, n13);
        return(_put_value(n14, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3652, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:224:73)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3652, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:224:73)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3652, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:224:73)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3652, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:224:73)");
    }
}
std::tuple<int,std::string> m3837(const std::tuple<int,std::string>& n11)
{
    try
    {
        return(n11);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3837, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3837, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3837, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3837, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3668()
{
    try
    {
        std::tuple<int,std::string> n16 = std::make_tuple( 2
        , std::string("y", 1) );
        std::tuple<int,std::string> n15 = std::make_tuple( 1
        , std::string("x", 1) );
        std::vector<std::tuple<int,std::string>> n17 = {n15,n16};
        std::vector<std::tuple<int,std::string>> n18 = morloc_vec_map( std::bind(m3837,
        std::placeholders::_1)
        , n17 );
        std::tuple<int,std::string> n13 = std::make_tuple( 2
        , std::string("y", 1) );
        std::tuple<int,std::string> n12 = std::make_tuple( 1
        , std::string("x", 1) );
        std::vector<std::tuple<int,std::string>> n14 = {n12,n13};
        bool n19 = morloc_vec_eq(n14, n18);
        return(_put_value(n19, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3668, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:225:88)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3668, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:225:88)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3668, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:225:88)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3668, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:225:88)");
    }
}
std::tuple<int,std::string> m3842(const std::tuple<int,std::string>& n12)
{
    try
    {
        return(n12);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=3842, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=3842, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=3842, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=3842, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m3690()
{
    try
    {
        std::tuple<int,std::string> n17 = std::make_tuple( 2
        , std::string("z", 1) );
        std::tuple<int,std::string> n16 = std::make_tuple( 1
        , std::string("x", 1) );
        std::vector<std::tuple<int,std::string>> n18 = {n16,n17};
        std::vector<std::tuple<int,std::string>> n19 = morloc_vec_map( std::bind(m3842,
        std::placeholders::_1)
        , n18 );
        std::tuple<int,std::string> n14 = std::make_tuple( 2
        , std::string("y", 1) );
        std::tuple<int,std::string> n13 = std::make_tuple( 1
        , std::string("x", 1) );
        std::vector<std::tuple<int,std::string>> n15 = {n13,n14};
        bool n20 = morloc_vec_eq(n15, n19);
        return(_put_value(n20, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3690, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:226:88)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3690, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:226:88)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3690, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:226:88)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3690, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:226:88)");
    }
}
int m3871(const int& n60, const int& n61)
{
    try
    {
        int n62 = (n60 + n61);
        return(n62);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3871, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:188:36)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3871, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:188:36)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3871, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:188:36)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3871, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:188:36)");
    }
}

uint8_t* m3869()
{
    try
    {
        std::vector<int> n63 = {1,2,3,4,5};
        int n64 = morloc_vec_fold(m3871, 0, n63);
        return(_put_value(n64, mlc_schema_table[2]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3869, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:188:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3869, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:188:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3869, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:188:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3869, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:188:31)");
    }
}
int8_t m3887(const int8_t& n62, const int8_t& n63)
{
    try
    {
        int8_t n64 = (n62 + n63);
        return(n64);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3887, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:189:35)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3887, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:189:35)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3887, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:189:35)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3887, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:189:35)");
    }
}

uint8_t* m3885()
{
    try
    {
        std::vector<int8_t> n65 = {static_cast<int8_t>(1)
        ,static_cast<int8_t>(2)
        ,static_cast<int8_t>(3)
        ,static_cast<int8_t>(4)
        ,static_cast<int8_t>(5)};
        int8_t n66 = morloc_vec_fold(m3887, static_cast<int8_t>(0), n65);
        return(_put_value(n66, mlc_schema_table[3]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3885, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:189:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3885, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:189:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3885, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:189:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3885, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:189:30)");
    }
}
int16_t m3903(const int16_t& n64, const int16_t& n65)
{
    try
    {
        int16_t n66 = (n64 + n65);
        return(n66);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3903, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:190:36)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3903, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:190:36)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3903, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:190:36)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3903, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:190:36)");
    }
}

uint8_t* m3901()
{
    try
    {
        std::vector<int16_t> n67 = {static_cast<int16_t>(1)
        ,static_cast<int16_t>(2)
        ,static_cast<int16_t>(3)
        ,static_cast<int16_t>(4)
        ,static_cast<int16_t>(5)};
        int16_t n68 = morloc_vec_fold(m3903, static_cast<int16_t>(0), n67);
        return(_put_value(n68, mlc_schema_table[4]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3901, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:190:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3901, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:190:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3901, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:190:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3901, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:190:31)");
    }
}
int32_t m3919(const int32_t& n66, const int32_t& n67)
{
    try
    {
        int32_t n68 = (n66 + n67);
        return(n68);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3919, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:191:36)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3919, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:191:36)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3919, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:191:36)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3919, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:191:36)");
    }
}

uint8_t* m3917()
{
    try
    {
        std::vector<int32_t> n69 = {static_cast<int32_t>(1)
        ,static_cast<int32_t>(2)
        ,static_cast<int32_t>(3)
        ,static_cast<int32_t>(4)
        ,static_cast<int32_t>(5)};
        int32_t n70 = morloc_vec_fold(m3919, static_cast<int32_t>(0), n69);
        return(_put_value(n70, mlc_schema_table[5]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3917, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:191:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3917, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:191:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3917, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:191:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3917, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:191:31)");
    }
}
int64_t m3935(const int64_t& n68, const int64_t& n69)
{
    try
    {
        int64_t n70 = (n68 + n69);
        return(n70);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3935, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:192:36)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3935, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:192:36)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3935, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:192:36)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3935, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:192:36)");
    }
}

uint8_t* m3933()
{
    try
    {
        std::vector<int64_t> n71 = {static_cast<int64_t>(1)
        ,static_cast<int64_t>(2)
        ,static_cast<int64_t>(3)
        ,static_cast<int64_t>(4)
        ,static_cast<int64_t>(5)};
        int64_t n72 = morloc_vec_fold(m3935, static_cast<int64_t>(0), n71);
        return(_put_value(n72, mlc_schema_table[6]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3933, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:192:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3933, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:192:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3933, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:192:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3933, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:192:31)");
    }
}
uint8_t m3951(const uint8_t& n70, const uint8_t& n71)
{
    try
    {
        uint8_t n72 = (n70 + n71);
        return(n72);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3951, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:193:35)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3951, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:193:35)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3951, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:193:35)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3951, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:193:35)");
    }
}

uint8_t* m3949()
{
    try
    {
        std::vector<uint8_t> n73 = {static_cast<uint8_t>(1)
        ,static_cast<uint8_t>(2)
        ,static_cast<uint8_t>(3)
        ,static_cast<uint8_t>(4)
        ,static_cast<uint8_t>(5)};
        uint8_t n74 = morloc_vec_fold(m3951, static_cast<uint8_t>(0), n73);
        return(_put_value(n74, mlc_schema_table[7]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3949, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:193:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3949, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:193:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3949, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:193:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3949, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:193:30)");
    }
}
uint16_t m3967(const uint16_t& n72, const uint16_t& n73)
{
    try
    {
        uint16_t n74 = (n72 + n73);
        return(n74);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3967, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:194:36)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3967, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:194:36)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3967, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:194:36)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3967, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:194:36)");
    }
}

uint8_t* m3965()
{
    try
    {
        std::vector<uint16_t> n75 = {static_cast<uint16_t>(1)
        ,static_cast<uint16_t>(2)
        ,static_cast<uint16_t>(3)
        ,static_cast<uint16_t>(4)
        ,static_cast<uint16_t>(5)};
        uint16_t n76 = morloc_vec_fold(m3967, static_cast<uint16_t>(0), n75);
        return(_put_value(n76, mlc_schema_table[8]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3965, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:194:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3965, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:194:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3965, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:194:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3965, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:194:31)");
    }
}
uint32_t m3983(const uint32_t& n74, const uint32_t& n75)
{
    try
    {
        uint32_t n76 = (n74 + n75);
        return(n76);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3983, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:195:36)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3983, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:195:36)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3983, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:195:36)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3983, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:195:36)");
    }
}

uint8_t* m3981()
{
    try
    {
        std::vector<uint32_t> n77 = {static_cast<uint32_t>(1)
        ,static_cast<uint32_t>(2)
        ,static_cast<uint32_t>(3)
        ,static_cast<uint32_t>(4)
        ,static_cast<uint32_t>(5)};
        uint32_t n78 = morloc_vec_fold(m3983, static_cast<uint32_t>(0), n77);
        return(_put_value(n78, mlc_schema_table[9]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3981, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:195:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3981, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:195:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3981, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:195:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3981, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:195:31)");
    }
}
uint64_t m3999(const uint64_t& n76, const uint64_t& n77)
{
    try
    {
        uint64_t n78 = (n76 + n77);
        return(n78);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3999, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:196:36)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3999, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:196:36)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3999, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:196:36)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3999, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:196:36)");
    }
}

uint8_t* m3997()
{
    try
    {
        std::vector<uint64_t> n79 = {static_cast<uint64_t>(1)
        ,static_cast<uint64_t>(2)
        ,static_cast<uint64_t>(3)
        ,static_cast<uint64_t>(4)
        ,static_cast<uint64_t>(5)};
        uint64_t n80 = morloc_vec_fold(m3999, static_cast<uint64_t>(0), n79);
        return(_put_value(n80, mlc_schema_table[10]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=3997, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:196:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=3997, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:196:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=3997, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:196:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=3997, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:196:31)");
    }
}
float m4015(const float& n78, const float& n79)
{
    try
    {
        float n80 = (n78 + n79);
        return(n80);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4015, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:197:36)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4015, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:197:36)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4015, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:197:36)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4015, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:197:36)");
    }
}

uint8_t* m4013()
{
    try
    {
        std::vector<float> n81 = {static_cast<float>(1.0)
        ,static_cast<float>(2.0)
        ,static_cast<float>(3.0)
        ,static_cast<float>(4.0)
        ,static_cast<float>(5.0)};
        float n82 = morloc_vec_fold(m4015, static_cast<float>(0.0), n81);
        return(_put_value(n82, mlc_schema_table[11]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4013, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:197:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4013, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:197:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4013, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:197:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4013, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:197:31)");
    }
}
double m4031(const double& n80, const double& n81)
{
    try
    {
        double n82 = (n80 + n81);
        return(n82);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4031, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:198:36)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4031, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:198:36)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4031, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:198:36)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4031, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:198:36)");
    }
}

uint8_t* m4029()
{
    try
    {
        std::vector<double> n83 = {1.0,2.0,3.0,4.0,5.0};
        double n84 = morloc_vec_fold(m4031, 0.0, n83);
        return(_put_value(n84, mlc_schema_table[12]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4029, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:198:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4029, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:198:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4029, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:198:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4029, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:198:31)");
    }
}
double m4047(const double& n82, const double& n83)
{
    try
    {
        double n84 = (n82 + n83);
        return(n84);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4047, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:199:37)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4047, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:199:37)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4047, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:199:37)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4047, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:199:37)");
    }
}

uint8_t* m4045()
{
    try
    {
        std::vector<double> n85 = {1.0,2.0,3.0,4.0,5.0};
        double n86 = morloc_vec_fold(m4047, 0.0, n85);
        return(_put_value(n86, mlc_schema_table[12]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4045, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:199:32)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4045, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:199:32)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4045, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:199:32)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4045, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:199:32)");
    }
}
int m4061(const int& n84, const int& n85)
{
    try
    {
        int n86 = (n84 + n85);
        return(n86);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4061, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:200:38)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4061, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:200:38)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4061, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:200:38)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4061, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:200:38)");
    }
}

uint8_t* m4059()
{
    try
    {
        std::vector<int> n87 = {};
        int n88 = morloc_vec_fold(m4061, 0, n87);
        return(_put_value(n88, mlc_schema_table[2]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4059, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:200:33)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4059, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:200:33)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4059, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:200:33)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4059, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:200:33)");
    }
}
int m4163(const int& n13)
{
    try
    {
        int n14 = (n13 * 2);
        return(n14);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4163, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4163, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4163, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4163, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:30)");
    }
}

uint8_t* m4161()
{
    try
    {
        std::vector<int> n15 = {1,2,3};
        std::vector<int> n16 = morloc_vec_map( std::bind(m4163,
        std::placeholders::_1)
        , n15 );
        return(_put_value(n16, mlc_schema_table[13]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4161, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4161, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4161, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4161, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:26)");
    }
}
int m4502(const int& n14)
{
    try
    {
        return(n14);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4502, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4502, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4502, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4502, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4172()
{
    try
    {
        std::vector<int> n15 = {2,4,6};
        std::vector<int> n16 = morloc_vec_map( std::bind(m4502,
        std::placeholders::_1)
        , n15 );
        return(_put_value(n16, mlc_schema_table[13]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4172, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:66)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4172, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:66)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4172, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:66)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4172, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:66)");
    }
}
int8_t m4185(const int8_t& n15)
{
    try
    {
        int8_t n16 = (n15 * static_cast<int8_t>(2));
        return(n16);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4185, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:29)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4185, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:29)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4185, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:29)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4185, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:29)");
    }
}

uint8_t* m4183()
{
    try
    {
        std::vector<int8_t> n17 = {static_cast<int8_t>(1)
        ,static_cast<int8_t>(2)
        ,static_cast<int8_t>(3)};
        std::vector<int8_t> n18 = morloc_vec_map( std::bind(m4185,
        std::placeholders::_1)
        , n17 );
        return(_put_value(n18, mlc_schema_table[14]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4183, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:25)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4183, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:25)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4183, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:25)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4183, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:25)");
    }
}
int8_t m4513(const int8_t& n16)
{
    try
    {
        return(n16);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4513, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4513, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4513, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4513, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4194()
{
    try
    {
        std::vector<int8_t> n17 = {static_cast<int8_t>(2)
        ,static_cast<int8_t>(4)
        ,static_cast<int8_t>(6)};
        std::vector<int8_t> n18 = morloc_vec_map( std::bind(m4513,
        std::placeholders::_1)
        , n17 );
        return(_put_value(n18, mlc_schema_table[14]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4194, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:64)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4194, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:64)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4194, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:64)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4194, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:64)");
    }
}
int16_t m4207(const int16_t& n17)
{
    try
    {
        int16_t n18 = (n17 * static_cast<int16_t>(2));
        return(n18);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4207, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4207, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4207, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4207, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:30)");
    }
}

uint8_t* m4205()
{
    try
    {
        std::vector<int16_t> n19 = {static_cast<int16_t>(1)
        ,static_cast<int16_t>(2)
        ,static_cast<int16_t>(3)};
        std::vector<int16_t> n20 = morloc_vec_map( std::bind(m4207,
        std::placeholders::_1)
        , n19 );
        return(_put_value(n20, mlc_schema_table[15]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4205, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4205, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4205, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4205, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:26)");
    }
}
int16_t m4524(const int16_t& n18)
{
    try
    {
        return(n18);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4524, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4524, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4524, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4524, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4216()
{
    try
    {
        std::vector<int16_t> n19 = {static_cast<int16_t>(2)
        ,static_cast<int16_t>(4)
        ,static_cast<int16_t>(6)};
        std::vector<int16_t> n20 = morloc_vec_map( std::bind(m4524,
        std::placeholders::_1)
        , n19 );
        return(_put_value(n20, mlc_schema_table[15]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4216, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:66)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4216, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:66)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4216, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:66)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4216, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:66)");
    }
}
int32_t m4229(const int32_t& n19)
{
    try
    {
        int32_t n20 = (n19 * static_cast<int32_t>(2));
        return(n20);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4229, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4229, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4229, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4229, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:30)");
    }
}

uint8_t* m4227()
{
    try
    {
        std::vector<int32_t> n21 = {static_cast<int32_t>(1)
        ,static_cast<int32_t>(2)
        ,static_cast<int32_t>(3)};
        std::vector<int32_t> n22 = morloc_vec_map( std::bind(m4229,
        std::placeholders::_1)
        , n21 );
        return(_put_value(n22, mlc_schema_table[16]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4227, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4227, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4227, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4227, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:26)");
    }
}
int32_t m4535(const int32_t& n20)
{
    try
    {
        return(n20);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4535, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4535, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4535, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4535, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4238()
{
    try
    {
        std::vector<int32_t> n21 = {static_cast<int32_t>(2)
        ,static_cast<int32_t>(4)
        ,static_cast<int32_t>(6)};
        std::vector<int32_t> n22 = morloc_vec_map( std::bind(m4535,
        std::placeholders::_1)
        , n21 );
        return(_put_value(n22, mlc_schema_table[16]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4238, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:66)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4238, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:66)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4238, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:66)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4238, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:66)");
    }
}
int64_t m4251(const int64_t& n21)
{
    try
    {
        int64_t n22 = (n21 * static_cast<int64_t>(2));
        return(n22);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4251, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4251, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4251, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4251, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:30)");
    }
}

uint8_t* m4249()
{
    try
    {
        std::vector<int64_t> n23 = {static_cast<int64_t>(1)
        ,static_cast<int64_t>(2)
        ,static_cast<int64_t>(3)};
        std::vector<int64_t> n24 = morloc_vec_map( std::bind(m4251,
        std::placeholders::_1)
        , n23 );
        return(_put_value(n24, mlc_schema_table[17]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4249, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4249, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4249, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4249, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:26)");
    }
}
int64_t m4546(const int64_t& n22)
{
    try
    {
        return(n22);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4546, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4546, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4546, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4546, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4260()
{
    try
    {
        std::vector<int64_t> n23 = {static_cast<int64_t>(2)
        ,static_cast<int64_t>(4)
        ,static_cast<int64_t>(6)};
        std::vector<int64_t> n24 = morloc_vec_map( std::bind(m4546,
        std::placeholders::_1)
        , n23 );
        return(_put_value(n24, mlc_schema_table[17]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4260, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:66)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4260, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:66)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4260, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:66)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4260, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:66)");
    }
}
uint8_t m4273(const uint8_t& n23)
{
    try
    {
        uint8_t n24 = (n23 * static_cast<uint8_t>(2));
        return(n24);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4273, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:29)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4273, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:29)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4273, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:29)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4273, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:29)");
    }
}

uint8_t* m4271()
{
    try
    {
        std::vector<uint8_t> n25 = {static_cast<uint8_t>(1)
        ,static_cast<uint8_t>(2)
        ,static_cast<uint8_t>(3)};
        std::vector<uint8_t> n26 = morloc_vec_map( std::bind(m4273,
        std::placeholders::_1)
        , n25 );
        return(_put_value(n26, mlc_schema_table[18]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4271, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:25)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4271, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:25)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4271, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:25)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4271, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:25)");
    }
}
uint8_t m4557(const uint8_t& n24)
{
    try
    {
        return(n24);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4557, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4557, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4557, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4557, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4282()
{
    try
    {
        std::vector<uint8_t> n25 = {static_cast<uint8_t>(2)
        ,static_cast<uint8_t>(4)
        ,static_cast<uint8_t>(6)};
        std::vector<uint8_t> n26 = morloc_vec_map( std::bind(m4557,
        std::placeholders::_1)
        , n25 );
        return(_put_value(n26, mlc_schema_table[18]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4282, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:64)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4282, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:64)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4282, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:64)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4282, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:64)");
    }
}
uint16_t m4295(const uint16_t& n25)
{
    try
    {
        uint16_t n26 = (n25 * static_cast<uint16_t>(2));
        return(n26);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4295, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4295, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4295, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4295, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:30)");
    }
}

uint8_t* m4293()
{
    try
    {
        std::vector<uint16_t> n27 = {static_cast<uint16_t>(1)
        ,static_cast<uint16_t>(2)
        ,static_cast<uint16_t>(3)};
        std::vector<uint16_t> n28 = morloc_vec_map( std::bind(m4295,
        std::placeholders::_1)
        , n27 );
        return(_put_value(n28, mlc_schema_table[19]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4293, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4293, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4293, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4293, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:26)");
    }
}
uint16_t m4568(const uint16_t& n26)
{
    try
    {
        return(n26);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4568, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4568, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4568, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4568, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4304()
{
    try
    {
        std::vector<uint16_t> n27 = {static_cast<uint16_t>(2)
        ,static_cast<uint16_t>(4)
        ,static_cast<uint16_t>(6)};
        std::vector<uint16_t> n28 = morloc_vec_map( std::bind(m4568,
        std::placeholders::_1)
        , n27 );
        return(_put_value(n28, mlc_schema_table[19]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4304, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:66)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4304, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:66)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4304, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:66)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4304, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:66)");
    }
}
uint32_t m4317(const uint32_t& n27)
{
    try
    {
        uint32_t n28 = (n27 * static_cast<uint32_t>(2));
        return(n28);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4317, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4317, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4317, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4317, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:30)");
    }
}

uint8_t* m4315()
{
    try
    {
        std::vector<uint32_t> n29 = {static_cast<uint32_t>(1)
        ,static_cast<uint32_t>(2)
        ,static_cast<uint32_t>(3)};
        std::vector<uint32_t> n30 = morloc_vec_map( std::bind(m4317,
        std::placeholders::_1)
        , n29 );
        return(_put_value(n30, mlc_schema_table[20]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4315, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4315, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4315, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4315, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:26)");
    }
}
uint32_t m4579(const uint32_t& n28)
{
    try
    {
        return(n28);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4579, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4579, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4579, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4579, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4326()
{
    try
    {
        std::vector<uint32_t> n29 = {static_cast<uint32_t>(2)
        ,static_cast<uint32_t>(4)
        ,static_cast<uint32_t>(6)};
        std::vector<uint32_t> n30 = morloc_vec_map( std::bind(m4579,
        std::placeholders::_1)
        , n29 );
        return(_put_value(n30, mlc_schema_table[20]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4326, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:66)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4326, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:66)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4326, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:66)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4326, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:66)");
    }
}
uint64_t m4339(const uint64_t& n29)
{
    try
    {
        uint64_t n30 = (n29 * static_cast<uint64_t>(2));
        return(n30);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4339, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4339, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4339, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4339, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:30)");
    }
}

uint8_t* m4337()
{
    try
    {
        std::vector<uint64_t> n31 = {static_cast<uint64_t>(1)
        ,static_cast<uint64_t>(2)
        ,static_cast<uint64_t>(3)};
        std::vector<uint64_t> n32 = morloc_vec_map( std::bind(m4339,
        std::placeholders::_1)
        , n31 );
        return(_put_value(n32, mlc_schema_table[21]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4337, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4337, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4337, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4337, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:26)");
    }
}
uint64_t m4590(const uint64_t& n30)
{
    try
    {
        return(n30);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4590, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4590, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4590, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4590, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4348()
{
    try
    {
        std::vector<uint64_t> n31 = {static_cast<uint64_t>(2)
        ,static_cast<uint64_t>(4)
        ,static_cast<uint64_t>(6)};
        std::vector<uint64_t> n32 = morloc_vec_map( std::bind(m4590,
        std::placeholders::_1)
        , n31 );
        return(_put_value(n32, mlc_schema_table[21]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4348, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:66)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4348, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:66)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4348, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:66)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4348, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:66)");
    }
}
float m4361(const float& n31)
{
    try
    {
        float n32 = (n31 * static_cast<float>(2.0));
        return(n32);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4361, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4361, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4361, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4361, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:30)");
    }
}

uint8_t* m4359()
{
    try
    {
        std::vector<float> n33 = {static_cast<float>(1.0)
        ,static_cast<float>(2.0)
        ,static_cast<float>(3.0)};
        std::vector<float> n34 = morloc_vec_map( std::bind(m4361,
        std::placeholders::_1)
        , n33 );
        return(_put_value(n34, mlc_schema_table[22]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4359, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4359, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4359, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4359, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:26)");
    }
}
float m4601(const float& n32)
{
    try
    {
        return(n32);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4601, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4601, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4601, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4601, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4370()
{
    try
    {
        std::vector<float> n33 = {static_cast<float>(2.0)
        ,static_cast<float>(4.0)
        ,static_cast<float>(6.0)};
        std::vector<float> n34 = morloc_vec_map( std::bind(m4601,
        std::placeholders::_1)
        , n33 );
        return(_put_value(n34, mlc_schema_table[22]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4370, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:74)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4370, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:74)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4370, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:74)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4370, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:74)");
    }
}
double m4383(const double& n33)
{
    try
    {
        double n34 = (n33 * 2.0);
        return(n34);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4383, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4383, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4383, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4383, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:30)");
    }
}

uint8_t* m4381()
{
    try
    {
        std::vector<double> n35 = {1.0,2.0,3.0};
        std::vector<double> n36 = morloc_vec_map( std::bind(m4383,
        std::placeholders::_1)
        , n35 );
        return(_put_value(n36, mlc_schema_table[23]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4381, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4381, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4381, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4381, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:26)");
    }
}
double m4612(const double& n34)
{
    try
    {
        return(n34);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4612, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4612, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4612, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4612, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4392()
{
    try
    {
        std::vector<double> n35 = {2.0,4.0,6.0};
        std::vector<double> n36 = morloc_vec_map( std::bind(m4612,
        std::placeholders::_1)
        , n35 );
        return(_put_value(n36, mlc_schema_table[23]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4392, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:74)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4392, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:74)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4392, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:74)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4392, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:74)");
    }
}
double m4405(const double& n35)
{
    try
    {
        double n36 = (n35 * 2.0);
        return(n36);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4405, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4405, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4405, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4405, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:31)");
    }
}

uint8_t* m4403()
{
    try
    {
        std::vector<double> n37 = {1.0,2.0,3.0};
        std::vector<double> n38 = morloc_vec_map( std::bind(m4405,
        std::placeholders::_1)
        , n37 );
        return(_put_value(n38, mlc_schema_table[23]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4403, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:27)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4403, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:27)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4403, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:27)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4403, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:27)");
    }
}
double m4623(const double& n36)
{
    try
    {
        return(n36);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4623, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4623, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4623, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4623, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4414()
{
    try
    {
        std::vector<double> n37 = {2.0,4.0,6.0};
        std::vector<double> n38 = morloc_vec_map( std::bind(m4623,
        std::placeholders::_1)
        , n37 );
        return(_put_value(n38, mlc_schema_table[23]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4414, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:76)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4414, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:76)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4414, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:76)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4414, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:76)");
    }
}
bool m4427(const bool& n37)
{
    try
    {
        return(n37);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4427, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:34)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4427, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:34)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4427, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:34)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4427, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:34)");
    }
}

uint8_t* m4425()
{
    try
    {
        std::vector<bool> n38 = {true,false,true};
        std::vector<bool> n39 = morloc_vec_map( std::bind(m4427,
        std::placeholders::_1)
        , n38 );
        return(_put_value(n39, mlc_schema_table[24]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4425, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:30)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4425, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:30)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4425, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:30)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4425, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:30)");
    }
}
bool m4636(const bool& n38)
{
    try
    {
        return(n38);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4636, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4636, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4636, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4636, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4432()
{
    try
    {
        std::vector<bool> n39 = {true,false,true};
        std::vector<bool> n40 = morloc_vec_map( std::bind(m4636,
        std::placeholders::_1)
        , n39 );
        return(_put_value(n40, mlc_schema_table[24]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4432, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:78)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4432, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:78)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4432, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:78)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4432, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:78)");
    }
}
std::string m4445(const std::string& n39)
{
    try
    {
        return(n39);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4445, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:33)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4445, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:33)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4445, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:33)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4445, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:33)");
    }
}

uint8_t* m4443()
{
    try
    {
        std::vector<std::string> n40 = {std::string("a", 1)
        ,std::string("b", 1)
        ,std::string("c", 1)};
        std::vector<std::string> n41 = morloc_vec_map( std::bind(m4445,
        std::placeholders::_1)
        , n40 );
        return(_put_value(n41, mlc_schema_table[25]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4443, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:29)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4443, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:29)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4443, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:29)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4443, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:29)");
    }
}
std::string m4649(const std::string& n40)
{
    try
    {
        return(n40);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4649, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4649, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4649, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4649, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4450()
{
    try
    {
        std::vector<std::string> n41 = {std::string("a", 1)
        ,std::string("b", 1)
        ,std::string("c", 1)};
        std::vector<std::string> n42 = morloc_vec_map( std::bind(m4649,
        std::placeholders::_1)
        , n41 );
        return(_put_value(n42, mlc_schema_table[25]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4450, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:72)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4450, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:72)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4450, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:72)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4450, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:72)");
    }
}
std::tuple<int,std::string> m4463(const std::tuple<int,std::string>& n41)
{
    try
    {
        return(n41);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4463, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:39)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4463, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:39)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4463, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:39)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4463, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:39)");
    }
}

uint8_t* m4461()
{
    try
    {
        std::tuple<int,std::string> n43 = std::make_tuple( 2
        , std::string("b", 1) );
        std::tuple<int,std::string> n42 = std::make_tuple( 1
        , std::string("a", 1) );
        std::vector<std::tuple<int,std::string>> n44 = {n42,n43};
        std::vector<std::tuple<int,std::string>> n45 = morloc_vec_map( std::bind(m4463,
        std::placeholders::_1)
        , n44 );
        return(_put_value(n45, mlc_schema_table[26]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4461, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:35)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4461, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:35)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4461, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:35)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4461, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:35)");
    }
}
std::tuple<int,std::string> m4662(const std::tuple<int,std::string>& n42)
{
    try
    {
        return(n42);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4662, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4662, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4662, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4662, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4471()
{
    try
    {
        std::tuple<int,std::string> n44 = std::make_tuple( 2
        , std::string("b", 1) );
        std::tuple<int,std::string> n43 = std::make_tuple( 1
        , std::string("a", 1) );
        std::vector<std::tuple<int,std::string>> n45 = {n43,n44};
        std::vector<std::tuple<int,std::string>> n46 = morloc_vec_map( std::bind(m4662,
        std::placeholders::_1)
        , n45 );
        return(_put_value(n46, mlc_schema_table[26]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4471, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:90)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4471, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:90)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4471, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:90)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4471, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:90)");
    }
}
int m4485(const int& n43)
{
    try
    {
        int n44 = (n43 * 2);
        return(n44);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4485, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:32)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4485, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:32)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4485, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:32)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4485, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:32)");
    }
}

uint8_t* m4483()
{
    try
    {
        std::vector<int> n45 = {};
        std::vector<int> n46 = morloc_vec_map( std::bind(m4485,
        std::placeholders::_1)
        , n45 );
        return(_put_value(n46, mlc_schema_table[27]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4483, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:28)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4483, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:28)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4483, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:28)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4483, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:28)");
    }
}
int m4667(const int& n44)
{
    try
    {
        return(n44);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4667, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4667, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4667, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4667, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4491()
{
    try
    {
        std::vector<int> n45 = {};
        std::vector<int> n46 = morloc_vec_map( std::bind(m4667,
        std::placeholders::_1)
        , n45 );
        return(_put_value(n46, mlc_schema_table[27]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4491, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:61)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4491, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:61)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4491, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:61)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4491, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:61)");
    }
}
uint8_t* m4694()
{
    try
    {
        std::vector<double> n1 = morloc_fill1(4.0, 0);
        return(_put_value(n1, mlc_schema_table[28]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4694, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:149:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4694, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:149:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4694, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:149:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4694, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:149:31)");
    }
}
double m4738(const double& n45)
{
    try
    {
        return(n45);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4738, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4738, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4738, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4738, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4698()
{
    try
    {
        std::vector<double> n46 = {};
        std::vector<double> n47 = morloc_vec_map( std::bind(m4738,
        std::placeholders::_1)
        , n46 );
        return(_put_value(n47, mlc_schema_table[28]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4698, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:149:65)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4698, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:149:65)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4698, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:149:65)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4698, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:149:65)");
    }
}
uint8_t* m4706()
{
    try
    {
        std::vector<double> n1 = morloc_fill1(7.0, 3);
        return(_put_value(n1, mlc_schema_table[23]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4706, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:150:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4706, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:150:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4706, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:150:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4706, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:150:31)");
    }
}
double m4749(const double& n46)
{
    try
    {
        return(n46);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4749, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4749, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4749, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4749, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4710()
{
    try
    {
        std::vector<double> n47 = {7.0,7.0,7.0};
        std::vector<double> n48 = morloc_vec_map( std::bind(m4749,
        std::placeholders::_1)
        , n47 );
        return(_put_value(n48, mlc_schema_table[23]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4710, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:150:65)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4710, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:150:65)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4710, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:150:65)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4710, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:150:65)");
    }
}
uint8_t* m4719()
{
    try
    {
        std::vector<double> n1 = morloc_fill1(-1.5, 4);
        return(_put_value(n1, mlc_schema_table[29]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4719, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:151:31)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4719, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:151:31)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4719, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:151:31)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4719, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:151:31)");
    }
}
double m4754(const double& n47)
{
    try
    {
        return(n47);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4754, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4754, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4754, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4754, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4723()
{
    try
    {
        std::vector<double> n48 = {-1.5,-1.5,-1.5,-1.5};
        std::vector<double> n49 = morloc_vec_map( std::bind(m4754,
        std::placeholders::_1)
        , n48 );
        return(_put_value(n49, mlc_schema_table[29]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4723, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:151:65)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4723, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:151:65)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4723, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:151:65)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4723, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:151:65)");
    }
}
uint8_t* m4781()
{
    try
    {
        std::vector<double> n1 = morloc_ones1(0);
        return(_put_value(n1, mlc_schema_table[28]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4781, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:141:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4781, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:141:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4781, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:141:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4781, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:141:26)");
    }
}
double m4836(const double& n48)
{
    try
    {
        return(n48);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4836, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4836, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4836, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4836, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4784()
{
    try
    {
        std::vector<double> n49 = {};
        std::vector<double> n50 = morloc_vec_map( std::bind(m4836,
        std::placeholders::_1)
        , n49 );
        return(_put_value(n50, mlc_schema_table[28]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4784, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:141:53)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4784, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:141:53)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4784, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:141:53)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4784, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:141:53)");
    }
}
uint8_t* m4792()
{
    try
    {
        std::vector<double> n1 = morloc_ones1(1);
        return(_put_value(n1, mlc_schema_table[30]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4792, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:142:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4792, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:142:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4792, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:142:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4792, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:142:26)");
    }
}
double m4847(const double& n49)
{
    try
    {
        return(n49);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4847, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4847, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4847, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4847, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4795()
{
    try
    {
        std::vector<double> n50 = {1.0};
        std::vector<double> n51 = morloc_vec_map( std::bind(m4847,
        std::placeholders::_1)
        , n50 );
        return(_put_value(n51, mlc_schema_table[30]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4795, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:142:53)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4795, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:142:53)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4795, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:142:53)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4795, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:142:53)");
    }
}
uint8_t* m4804()
{
    try
    {
        std::vector<double> n1 = morloc_ones1(3);
        return(_put_value(n1, mlc_schema_table[23]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4804, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:143:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4804, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:143:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4804, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:143:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4804, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:143:26)");
    }
}
double m4858(const double& n50)
{
    try
    {
        return(n50);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4858, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4858, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4858, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4858, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4807()
{
    try
    {
        std::vector<double> n51 = {1.0,1.0,1.0};
        std::vector<double> n52 = morloc_vec_map( std::bind(m4858,
        std::placeholders::_1)
        , n51 );
        return(_put_value(n52, mlc_schema_table[23]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4807, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:143:53)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4807, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:143:53)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4807, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:143:53)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4807, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:143:53)");
    }
}
uint8_t* m4816()
{
    try
    {
        std::vector<double> n1 = morloc_ones1(6);
        return(_put_value(n1, mlc_schema_table[31]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4816, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:144:26)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4816, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:144:26)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4816, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:144:26)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4816, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:144:26)");
    }
}
double m4863(const double& n51)
{
    try
    {
        return(n51);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4863, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4863, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4863, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4863, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4819()
{
    try
    {
        std::vector<double> n52 = {1.0,1.0,1.0,1.0,1.0,1.0};
        std::vector<double> n53 = morloc_vec_map( std::bind(m4863,
        std::placeholders::_1)
        , n52 );
        return(_put_value(n53, mlc_schema_table[31]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4819, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:144:53)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4819, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:144:53)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4819, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:144:53)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4819, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:144:53)");
    }
}
uint8_t* m4890()
{
    try
    {
        std::vector<double> n1 = morloc_zeros1(0);
        return(_put_value(n1, mlc_schema_table[28]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4890, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:133:27)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4890, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:133:27)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4890, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:133:27)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4890, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:133:27)");
    }
}
double m4945(const double& n52)
{
    try
    {
        return(n52);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4945, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4945, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4945, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4945, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4893()
{
    try
    {
        std::vector<double> n53 = {};
        std::vector<double> n54 = morloc_vec_map( std::bind(m4945,
        std::placeholders::_1)
        , n53 );
        return(_put_value(n54, mlc_schema_table[28]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4893, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:133:55)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4893, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:133:55)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4893, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:133:55)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4893, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:133:55)");
    }
}
uint8_t* m4901()
{
    try
    {
        std::vector<double> n1 = morloc_zeros1(1);
        return(_put_value(n1, mlc_schema_table[30]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4901, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:134:27)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4901, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:134:27)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4901, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:134:27)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4901, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:134:27)");
    }
}
double m4956(const double& n53)
{
    try
    {
        return(n53);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4956, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4956, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4956, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4956, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4904()
{
    try
    {
        std::vector<double> n54 = {0.0};
        std::vector<double> n55 = morloc_vec_map( std::bind(m4956,
        std::placeholders::_1)
        , n54 );
        return(_put_value(n55, mlc_schema_table[30]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4904, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:134:55)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4904, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:134:55)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4904, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:134:55)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4904, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:134:55)");
    }
}
uint8_t* m4913()
{
    try
    {
        std::vector<double> n1 = morloc_zeros1(3);
        return(_put_value(n1, mlc_schema_table[23]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4913, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:135:27)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4913, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:135:27)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4913, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:135:27)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4913, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:135:27)");
    }
}
double m4967(const double& n54)
{
    try
    {
        return(n54);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4967, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4967, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4967, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4967, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4916()
{
    try
    {
        std::vector<double> n55 = {0.0,0.0,0.0};
        std::vector<double> n56 = morloc_vec_map( std::bind(m4967,
        std::placeholders::_1)
        , n55 );
        return(_put_value(n56, mlc_schema_table[23]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4916, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:135:55)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4916, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:135:55)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4916, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:135:55)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4916, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:135:55)");
    }
}
uint8_t* m4925()
{
    try
    {
        std::vector<double> n1 = morloc_zeros1(6);
        return(_put_value(n1, mlc_schema_table[31]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4925, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:136:27)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4925, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:136:27)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4925, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:136:27)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4925, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:136:27)");
    }
}
double m4972(const double& n55)
{
    try
    {
        return(n55);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=4972, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=4972, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=4972, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=4972, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4928()
{
    try
    {
        std::vector<double> n56 = {0.0,0.0,0.0,0.0,0.0,0.0};
        std::vector<double> n57 = morloc_vec_map( std::bind(m4972,
        std::placeholders::_1)
        , n56 );
        return(_put_value(n57, mlc_schema_table[31]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4928, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:136:55)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4928, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:136:55)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4928, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:136:55)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4928, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:136:55)");
    }
}
int m5447(const int& n56)
{
    try
    {
        return(n56);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=5447, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=5447, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=5447, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=5447, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m4989()
{
    try
    {
        std::vector<int> n57 = {0,1,2,3,4,5,6,7,8,9};
        std::vector<int> n58 = morloc_vec_map( std::bind(m5447,
        std::placeholders::_1)
        , n57 );
        return(_put_value(n58, mlc_schema_table[32]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=4989, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:92:13)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=4989, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:92:13)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=4989, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:92:13)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=4989, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:92:13)");
    }
}
int m5452(const int& n58)
{
    try
    {
        return(n58);
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at id [cpp] (mid=5452, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at id [cpp] (mid=5452, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at id [cpp] (mid=5452, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at id [cpp] (mid=5452, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:26:9)");
    }
}

uint8_t* m5002()
{
    try
    {
        std::vector<int> n59 = {0,1,2,3};
        std::vector<int> n60 = morloc_vec_map( std::bind(m5452,
        std::placeholders::_1)
        , n59 );
        return(_put_value(n60, mlc_schema_table[33]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5002, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:93:14)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5002, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:93:14)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5002, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:93:14)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5002, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:93:14)");
    }
}
uint8_t* m5014(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::optional<int64_t> n58 = morloc_to_index(0);
        int n59 = morloc_vec_at(n58, n57);
        bool n60 = morloc_eq(n59, 0);
        return(_put_value(n60, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5014, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:94:43)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5014, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:94:43)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5014, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:94:43)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5014, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:94:43)");
    }
}
uint8_t* m5026(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::optional<int64_t> n58 = morloc_to_index(-1);
        int n59 = morloc_vec_at(n58, n57);
        bool n60 = morloc_eq(n59, 9);
        return(_put_value(n60, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5026, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:95:45)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5026, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:95:45)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5026, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:95:45)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5026, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:95:45)");
    }
}
uint8_t* m5038(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::optional<int64_t> n58 = morloc_to_index(-2);
        int n59 = morloc_vec_at(n58, n57);
        bool n60 = morloc_eq(n59, 8);
        return(_put_value(n60, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5038, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:96:45)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5038, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:96:45)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5038, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:96:45)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5038, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:96:45)");
    }
}
uint8_t* m5050(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        int n58 = (1 + 1);
        std::optional<int64_t> n59 = morloc_to_index(n58);
        int n60 = morloc_vec_at(n59, n57);
        bool n61 = morloc_eq(n60, 2);
        return(_put_value(n61, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5050, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:97:47)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5050, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:97:47)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5050, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:97:47)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5050, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:97:47)");
    }
}
uint8_t* m5065(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n58 = morloc_to_index(std::optional<int64_t>{});
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n62 = morloc_vec_eq(n61, n57);
        return(_put_value(n62, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5065, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:98:45)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5065, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:98:45)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5065, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:98:45)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5065, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:98:45)");
    }
}
uint8_t* m5079(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(0);
        std::optional<int64_t> n58 = morloc_to_index(0);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5079, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:99:48)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5079, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:99:48)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5079, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:99:48)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5079, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:99:48)");
    }
}
uint8_t* m5093(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(1);
        std::optional<int64_t> n58 = morloc_to_index(1);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5093, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:100:48)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5093, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:100:48)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5093, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:100:48)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5093, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:100:48)");
    }
}
uint8_t* m5107(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(0);
        std::optional<int64_t> n58 = morloc_to_index(1);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5107, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:101:48)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5107, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:101:48)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5107, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:101:48)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5107, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:101:48)");
    }
}
uint8_t* m5121(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {8,9};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(99);
        std::optional<int64_t> n58 = morloc_to_index(8);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5121, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:102:53)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5121, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:102:53)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5121, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:102:53)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5121, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:102:53)");
    }
}
uint8_t* m5137(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {6,7,8};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(-1);
        std::optional<int64_t> n58 = morloc_to_index(6);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5137, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:103:55)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5137, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:103:55)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5137, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:103:55)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5137, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:103:55)");
    }
}
uint8_t* m5154(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {0};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(-9);
        std::optional<int64_t> n58 = morloc_to_index(0);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5154, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:104:51)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5154, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:104:51)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5154, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:104:51)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5154, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:104:51)");
    }
}
uint8_t* m5169(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(-10);
        std::optional<int64_t> n58 = morloc_to_index(0);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5169, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:105:52)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5169, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:105:52)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5169, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:105:52)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5169, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:105:52)");
    }
}
uint8_t* m5183(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(0);
        std::optional<int64_t> n58 = morloc_to_index(-1);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5183, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:106:50)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5183, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:106:50)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5183, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:106:50)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5183, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:106:50)");
    }
}
uint8_t* m5197(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {9};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n58 = morloc_to_index(-1);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5197, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:107:49)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5197, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:107:49)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5197, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:107:49)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5197, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:107:49)");
    }
}
uint8_t* m5212(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::optional<int64_t> n64 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n63 = morloc_to_index(9);
        std::optional<int64_t> n62 = morloc_to_index(8);
        std::vector<int> n65 = morloc_vec_slice(n62, n63, n64, n57);
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(-1);
        std::optional<int64_t> n58 = morloc_to_index(-2);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n66 = morloc_vec_eq(n61, n65);
        return(_put_value(n66, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5212, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:108:60)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5212, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:108:60)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5212, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:108:60)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5212, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:108:60)");
    }
}
uint8_t* m5231(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {1};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(2);
        std::optional<int64_t> n58 = morloc_to_index(1);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5231, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:109:67)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5231, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:109:67)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5231, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:109:67)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5231, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:109:67)");
    }
}
uint8_t* m5246(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n58 = morloc_to_index(std::optional<int64_t>{});
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n62 = morloc_vec_eq(n61, n57);
        return(_put_value(n62, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5246, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:110:47)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5246, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:110:47)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5246, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:110:47)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5246, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:110:47)");
    }
}
uint8_t* m5260(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {0,2,4,6,8};
        std::optional<int64_t> n60 = morloc_to_index(2);
        std::optional<int64_t> n59 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n58 = morloc_to_index(std::optional<int64_t>{});
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5260, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:111:57)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5260, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:111:57)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5260, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:111:57)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5260, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:111:57)");
    }
}
uint8_t* m5279(const uint8_t* s59)
{
    try
    {
        std::vector<int> n59 = _get_value<std::vector<int>>(s59, mlc_schema_table[33]);
        std::vector<int> n64 = {3,2,1,0};
        std::optional<int64_t> n62 = morloc_to_index(-1);
        std::optional<int64_t> n61 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::vector<int> n63 = morloc_vec_slice(n60, n61, n62, n59);
        bool n65 = morloc_vec_eq(n63, n64);
        return(_put_value(n65, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5279, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:112:59)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5279, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:112:59)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5279, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:112:59)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5279, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:112:59)");
    }
}
uint8_t* m5297(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {9,7,5,3,1};
        std::optional<int64_t> n60 = morloc_to_index(-2);
        std::optional<int64_t> n59 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n58 = morloc_to_index(std::optional<int64_t>{});
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5297, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:113:59)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5297, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:113:59)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5297, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:113:59)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5297, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:113:59)");
    }
}
uint8_t* m5316(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {7,8,9};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n58 = morloc_to_index(7);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5316, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:114:53)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5316, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:114:53)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5316, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:114:53)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5316, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:114:53)");
    }
}
uint8_t* m5333(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {0,1,2};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(3);
        std::optional<int64_t> n58 = morloc_to_index(std::optional<int64_t>{});
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5333, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:115:53)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5333, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:115:53)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5333, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:115:53)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5333, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:115:53)");
    }
}
uint8_t* m5350(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {0,1,2};
        std::optional<int64_t> n60 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n59 = morloc_to_index(3);
        std::optional<int64_t> n58 = morloc_to_index(0);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5350, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:116:55)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5350, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:116:55)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5350, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:116:55)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5350, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:116:55)");
    }
}
uint8_t* m5367(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {7,8,9};
        std::optional<int64_t> n60 = morloc_to_index(1);
        std::optional<int64_t> n59 = morloc_to_index(std::optional<int64_t>{});
        std::optional<int64_t> n58 = morloc_to_index(7);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5367, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:117:55)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5367, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:117:55)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5367, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:117:55)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5367, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:117:55)");
    }
}
uint8_t* m5384(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {0,1,2};
        std::optional<int64_t> n60 = morloc_to_index(1);
        std::optional<int64_t> n59 = morloc_to_index(3);
        std::optional<int64_t> n58 = morloc_to_index(std::optional<int64_t>{});
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5384, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:118:55)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5384, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:118:55)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5384, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:118:55)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5384, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:118:55)");
    }
}
uint8_t* m5401(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {0,1,2};
        std::optional<int64_t> n60 = morloc_to_index(1);
        std::optional<int64_t> n59 = morloc_to_index(3);
        std::optional<int64_t> n58 = morloc_to_index(0);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5401, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:119:57)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5401, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:119:57)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5401, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:119:57)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5401, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:119:57)");
    }
}
uint8_t* m5418(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {2,4,6};
        std::optional<int64_t> n60 = morloc_to_index(2);
        std::optional<int64_t> n59 = morloc_to_index(7);
        std::optional<int64_t> n58 = morloc_to_index(2);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5418, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:120:57)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5418, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:120:57)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5418, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:120:57)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5418, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:120:57)");
    }
}
uint8_t* m5433(const uint8_t* s57)
{
    try
    {
        std::vector<int> n57 = _get_value<std::vector<int>>(s57, mlc_schema_table[32]);
        std::vector<int> n62 = {7,5,3};
        std::optional<int64_t> n60 = morloc_to_index(-2);
        std::optional<int64_t> n59 = morloc_to_index(2);
        std::optional<int64_t> n58 = morloc_to_index(7);
        std::vector<int> n61 = morloc_vec_slice(n58, n59, n60, n57);
        bool n63 = morloc_vec_eq(n61, n62);
        return(_put_value(n63, mlc_schema_table[1]));
    } catch (const std::exception& e)
      {
          throw std::runtime_error(std::string(e.what()) + "\n  at _ [cpp] (mid=5433, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:121:59)");
      }
    catch (const char* e)
    {
        throw std::runtime_error(std::string(e) + "\n  at _ [cpp] (mid=5433, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:121:59)");
    }
    catch (const std::string& e)
    {
        throw std::runtime_error(e + "\n  at _ [cpp] (mid=5433, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:121:59)");
    }
    catch (...)
    {
        throw std::runtime_error(std::string("An unknown error occurred") + "\n  at _ [cpp] (mid=5433, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:121:59)");
    }
}
// AUTO manifolds statements end



// AUTO dispatch start
uint8_t* local_dispatch(uint32_t mid, const uint8_t** args){
    switch(mid){
        case 1: return m1();
        case 3397: return m3397();
        case 3490: return m3490();
        case 3508: return m3508();
        case 3526: return m3526();
        case 3544: return m3544();
        case 3560: return m3560();
        case 3576: return m3576();
        case 3592: return m3592();
        case 3608: return m3608();
        case 3624: return m3624();
        case 3636: return m3636();
        case 3652: return m3652();
        case 3668: return m3668();
        case 3690: return m3690();
        case 3869: return m3869();
        case 3885: return m3885();
        case 3901: return m3901();
        case 3917: return m3917();
        case 3933: return m3933();
        case 3949: return m3949();
        case 3965: return m3965();
        case 3981: return m3981();
        case 3997: return m3997();
        case 4013: return m4013();
        case 4029: return m4029();
        case 4045: return m4045();
        case 4059: return m4059();
        case 4161: return m4161();
        case 4172: return m4172();
        case 4183: return m4183();
        case 4194: return m4194();
        case 4205: return m4205();
        case 4216: return m4216();
        case 4227: return m4227();
        case 4238: return m4238();
        case 4249: return m4249();
        case 4260: return m4260();
        case 4271: return m4271();
        case 4282: return m4282();
        case 4293: return m4293();
        case 4304: return m4304();
        case 4315: return m4315();
        case 4326: return m4326();
        case 4337: return m4337();
        case 4348: return m4348();
        case 4359: return m4359();
        case 4370: return m4370();
        case 4381: return m4381();
        case 4392: return m4392();
        case 4403: return m4403();
        case 4414: return m4414();
        case 4425: return m4425();
        case 4432: return m4432();
        case 4443: return m4443();
        case 4450: return m4450();
        case 4461: return m4461();
        case 4471: return m4471();
        case 4483: return m4483();
        case 4491: return m4491();
        case 4694: return m4694();
        case 4698: return m4698();
        case 4706: return m4706();
        case 4710: return m4710();
        case 4719: return m4719();
        case 4723: return m4723();
        case 4781: return m4781();
        case 4784: return m4784();
        case 4792: return m4792();
        case 4795: return m4795();
        case 4804: return m4804();
        case 4807: return m4807();
        case 4816: return m4816();
        case 4819: return m4819();
        case 4890: return m4890();
        case 4893: return m4893();
        case 4901: return m4901();
        case 4904: return m4904();
        case 4913: return m4913();
        case 4916: return m4916();
        case 4925: return m4925();
        case 4928: return m4928();
        case 4989: return m4989();
        case 5002: return m5002();
        case 5014: return m5014(args[0]);
        case 5026: return m5026(args[0]);
        case 5038: return m5038(args[0]);
        case 5050: return m5050(args[0]);
        case 5065: return m5065(args[0]);
        case 5079: return m5079(args[0]);
        case 5093: return m5093(args[0]);
        case 5107: return m5107(args[0]);
        case 5121: return m5121(args[0]);
        case 5137: return m5137(args[0]);
        case 5154: return m5154(args[0]);
        case 5169: return m5169(args[0]);
        case 5183: return m5183(args[0]);
        case 5197: return m5197(args[0]);
        case 5212: return m5212(args[0]);
        case 5231: return m5231(args[0]);
        case 5246: return m5246(args[0]);
        case 5260: return m5260(args[0]);
        case 5279: return m5279(args[0]);
        case 5297: return m5297(args[0]);
        case 5316: return m5316(args[0]);
        case 5333: return m5333(args[0]);
        case 5350: return m5350(args[0]);
        case 5367: return m5367(args[0]);
        case 5384: return m5384(args[0]);
        case 5401: return m5401(args[0]);
        case 5418: return m5418(args[0]);
        case 5433: return m5433(args[0]);
        default:
            std::ostringstream oss;
            oss << "Invalid local manifold id: " << mid;
            throw std::runtime_error(oss.str());
    }
}

uint8_t* remote_dispatch(uint32_t mid, const uint8_t** args){
    switch(mid){
        
        default:
            std::ostringstream oss;
            oss << "Invalid remote manifold id: " << mid;
            throw std::runtime_error(oss.str());
    }
}
// AUTO dispatch end


// Drain the debug-trace if --debug was compiled in. Concatenates the
// rendered morloc trace with the caught exception's message so the
// fail packet carries both. When --debug is off (or no frames were
// recorded), the drain returns NULL and we forward the message
// unchanged. Every manifold catch also appends its own "  at <name>
// [cpp] (mid=..., srcloc)" line to the message via string concat,
// so non-debug tracebacks still compose across pools.
static uint8_t* make_fail_packet_with_trace(const char* msg) {
    char* trace = morloc_debug_drain_frames();
    if (trace == NULL) {
        return make_fail_packet(msg);
    }
    std::string combined;
    combined.reserve(strlen(msg) + strlen(trace) + 2);
    combined.append(msg);
    combined.append("\n");
    combined.append(trace);
    free(trace);
    return make_fail_packet(combined.c_str());
}

// Wrappers to adapt compiler-generated dispatch functions to pool_dispatch_fn_t.
// These catch C++ exceptions so the C pool_main never sees them. External
// linkage so the (member-agnostic) host translation unit can register them.
uint8_t* cpp_local_dispatch(uint32_t mid, const uint8_t** args,
                                    size_t nargs, void* ctx) {
    (void)nargs; (void)ctx;
    // Free SHM from previous dispatch (result packet consumed by caller)
    _shm_tracker_flush();
    morloc_debug_flush_dispatch();
    try {
        return local_dispatch(mid, args);
    } catch (const MorlocPipeClosed& e) {
        // Broken pipe: a distinguished, non-abort failure. The nexus owns
        // fd 1 and decides the pipeline exit status.
        return make_fail_packet(e.what());
    } catch (const MorlocInfraError& e) {
        // Not recoverable and not the user's doing. Aborting rather than
        // returning a fail packet is what keeps it fatal across pools: a
        // caller's socket read fails, which it classifies as infrastructure
        // in turn, so no fail packet has to carry the classification.
        MLC_INTERNAL_ABORT(e.what());
    } catch (const std::exception& e) {
        return make_fail_packet_with_trace(e.what());
    } catch (const char* e) {
        // Codegen-emitted manifold catches usually normalize const char*
        // throws into std::runtime_error already, but a foreign helper
        // that throws outside a wrapped body reaches here directly.
        return make_fail_packet_with_trace(e ? e : "An unknown error occurred");
    } catch (const std::string& e) {
        return make_fail_packet_with_trace(e.c_str());
    } catch (...) {
        return make_fail_packet_with_trace("An unknown error occurred");
    }
}

uint8_t* cpp_remote_dispatch(uint32_t mid, const uint8_t** args,
                                     size_t nargs, void* ctx) {
    (void)nargs; (void)ctx;
    // Every entry in the tracker is a reference this pool owns: a block it
    // allocated, an argument it took a reference on, or a result reference
    // inherited from a callee. Anyone holding a packet that left this pool
    // holds a separate reference taken before it was sent. So releasing at
    // a dispatch boundary is safe here for the same reason it is safe in
    // the local dispatcher, and the two should not differ.
    _shm_tracker_flush();
    morloc_debug_flush_dispatch();
    try {
        return remote_dispatch(mid, args);
    } catch (const MorlocPipeClosed& e) {
        // Broken pipe: a distinguished, non-abort failure. The nexus owns
        // fd 1 and decides the pipeline exit status.
        return make_fail_packet(e.what());
    } catch (const MorlocInfraError& e) {
        // Not recoverable and not the user's doing. Aborting rather than
        // returning a fail packet is what keeps it fatal across pools: a
        // caller's socket read fails, which it classifies as infrastructure
        // in turn, so no fail packet has to carry the classification.
        MLC_INTERNAL_ABORT(e.what());
    } catch (const std::exception& e) {
        return make_fail_packet_with_trace(e.what());
    } catch (const char* e) {
        return make_fail_packet_with_trace(e ? e : "An unknown error occurred");
    } catch (const std::string& e) {
        return make_fail_packet_with_trace(e.c_str());
    } catch (...) {
        return make_fail_packet_with_trace("An unknown error occurred");
    }
}


// Registration entry called by the member-agnostic host translation unit.
// The host owns main() and the pool_main loop; this hook wires the C++
// member's dispatchers, concurrency policy, tmpdir, and schema table into the
// pool config. cpp_local_dispatch/cpp_remote_dispatch, g_tmpdir, POOL_THREADS,
// and _init_schemas all live in this (member) translation unit.
void cpp_register(pool_config_t* config, const char* tmpdir) {
    g_tmpdir = strdup(tmpdir);
    config->local_dispatch = cpp_local_dispatch;
    config->remote_dispatch = cpp_remote_dispatch;
    config->dispatch_ctx = NULL;
    config->concurrency = POOL_THREADS;
    config->initial_workers = 1;
    config->dynamic_scaling = true;
    _init_schemas();
}
