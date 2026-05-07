#include <string>
#include <iostream>
#include <sstream>
#include <functional>
#include <vector>
#include <algorithm>
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
#include <unistd.h>

#include <limits>
#include <utility>

char* g_tmpdir;

uint8_t* foreign_call(const char* socket_filename, size_t mid, ...) __attribute__((sentinel));

// AUTO include statements start
#include "/home/dev/.local/share/morloc/src/morloc/plane/default/root-cpp/core.hpp"
#include "/home/dev/.local/share/morloc/src/morloc/plane/default/vector-cpp/vector.hpp"
// AUTO include statements end

// Proper linking of cppmorloc requires it be included AFTER the custom modules
#include "mlc_arrow.hpp"
#include "cppmorloc.hpp"

#define PROPAGATE_ERROR(errmsg) \
    if(errmsg != NULL) { \
      char errmsg_buffer[MAX_ERRMSG_SIZE] = { 0 }; \
      snprintf(errmsg_buffer, MAX_ERRMSG_SIZE, "Error C++ pool (%s:%d in %s):\n%s" , __FILE__, __LINE__, __func__, errmsg); \
      free(errmsg); \
      throw std::runtime_error(errmsg_buffer); \
    }

#define PROPAGATE_FAIL_PACKET(errmsg) \
    if(errmsg != NULL){ \
        uint8_t* fail_packet_ = make_fail_packet(errmsg); \
        free(errmsg); \
        return fail_packet_; \
    }


// AUTO serialization statements start
static Schema* mlc_schema_table[8];
void _init_schemas() {
    static const char* _schema_strs[] = {
        "<std::tuple<$1,$2>>t2<int>j<int>j",
        "<bool>b",
        "<std::vector<$2>>a:0<double>f8",
        "<std::vector<$2>>a:3<double>f8",
        "<std::vector<$2>>a:4<double>f8",
        "<std::vector<$2>>a:8<double>f8",
        "<std::vector<$2>>a:1<double>f8",
        "<std::vector<$2>>a:6<double>f8",
    };
    for (int i = 0; i < 8; i++)
        mlc_schema_table[i] = parse_schema_cpp(_schema_strs[i]);
}
// AUTO serialization statements end



std::string interweave_strings(const std::vector<std::string>& first, const std::vector<std::string>& second)
{
    // Validate sizes - errors here indicate a bug in the morloc compiler
    if (first.size() != second.size() + 1) {
        throw std::invalid_argument("First list must have exactly 1 more element than second list");
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
struct ShmEntry { absptr_t ptr; Schema* schema; };
thread_local std::vector<ShmEntry> _shm_tracker;

static void _flush_shm_tracker() {
    for (auto& e : _shm_tracker) {
        char* err = NULL;
        // Only do recursive sub-freeing if we have a schema and this is
        // the last reference. NULL schema entries (from foreign_call result
        // tracking) just decrement the refcount.
        block_header_t* blk = (block_header_t*)((char*)e.ptr - sizeof(block_header_t));
        if (e.schema && blk->reference_count <= 1) {
            shfree_by_schema(e.ptr, e.schema, &err);
            if (err) { free(err); err = NULL; }
        }
        shfree(e.ptr, &err);
        if (err) { free(err); }
    }
    _shm_tracker.clear();
}

// Transforms a serialized value into a message ready for the socket
template <typename T>
uint8_t* _put_value(const T& value, Schema* schema) {

    if constexpr (std::is_same_v<T, mlc::ArrowTable>) {
        // Arrow export: move table data into SHM, build packet.
        // const_cast is safe here: the value is always a temporary from
        // a manifold call, never a truly const object.
        mlc::ArrowTable& tbl = const_cast<mlc::ArrowTable&>(value);
        relptr_t relptr = tbl.move_to_shm();

        uint8_t* packet = make_arrow_data_packet(relptr, schema);
        if (!packet) { throw std::runtime_error("Failed to create arrow data packet"); }

        char* err = nullptr;
        void* shm_ptr = rel2abs(relptr, &err);
        if (err) { free(err); }
        if (shm_ptr) { _shm_tracker.push_back({(absptr_t)shm_ptr, nullptr}); }
        return packet;
    } else {
        // Arrow dispatch: schema marker `T` (MORLOC_TABLE) routes through
        // mlc::ArrowTable. The legacy `<arrow>` hint has been retired.
        if (schema->type == MORLOC_TABLE) {
            throw std::runtime_error("Table schema but C++ type is not mlc::ArrowTable");
        }

        void* voidstar = nullptr;
        try {
            voidstar = toAnything(schema, value);
            relptr_t relptr = abs2rel_cpp(voidstar);

            char* errmsg = nullptr;
            uint8_t* packet = make_data_packet_auto(voidstar, relptr, schema, &errmsg);
            if (errmsg) {
                shfree_cpp(voidstar);
                PROPAGATE_ERROR(errmsg);
            }

            const morloc_packet_header_t* hdr = (const morloc_packet_header_t*)packet;
            if (hdr->command.data.source == PACKET_SOURCE_RPTR) {
                // SHM referenced by packet -- track for deferred cleanup
                _shm_tracker.push_back({(absptr_t)voidstar, schema});
            } else {
                // Data inlined in packet -- free SHM immediately
                char* free_err = NULL;
                shfree_by_schema((absptr_t)voidstar, schema, &free_err);
                if (free_err) { free(free_err); free_err = NULL; }
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
        // Arrow import: packet -> arrow_from_shm -> ArrowTable
        char* errmsg = nullptr;
        uint8_t* raw = get_morloc_data_packet_value(packet, schema, &errmsg);
        if (errmsg) { PROPAGATE_ERROR(errmsg); }

        const arrow_shm_header_t* hdr = (const arrow_shm_header_t*)raw;
        struct ArrowSchema as;
        struct ArrowArray aa;
        char* aerr = nullptr;
        arrow_from_shm(hdr, &as, &aa, &aerr);
        if (aerr) { PROPAGATE_ERROR(aerr); }

        char* ierr = nullptr;
        shincref((absptr_t)raw, &ierr);
        if (ierr) { free(ierr); }
        _shm_tracker.push_back({(absptr_t)raw, nullptr});

        return mlc::ArrowTable(std::move(as), std::move(aa));
    } else {
        if (format == PACKET_FORMAT_ARROW) {
            throw std::runtime_error("Arrow data but C++ type is not mlc::ArrowTable");
        }

        // Fast path: inline voidstar -- read directly from packet, no SHM needed
        if (source == PACKET_SOURCE_MESG && format == PACKET_FORMAT_VOIDSTAR) {
            const uint8_t* payload = packet + sizeof(morloc_packet_header_t) + header->offset;
            T* dummy = nullptr;
            return fromAnything(schema, (const void*)payload, dummy, (const void*)payload);
        }

        // SHM paths (RPTR or MESG+MSGPACK): existing logic
        bool is_rptr = (source == PACKET_SOURCE_RPTR);

        char* errmsg = NULL;
        uint8_t* voidstar = get_morloc_data_packet_value(packet, schema, &errmsg);
        if(errmsg != NULL) {
            PROPAGATE_ERROR(errmsg)
        }

        // For RPTR data, increment refcount so the owner's tracker flush
        // won't destroy data we may still need (e.g. forwarded packets).
        if (is_rptr) {
            char* incref_err = NULL;
            shincref((absptr_t)voidstar, &incref_err);
            if (incref_err) { free(incref_err); }
            _shm_tracker.push_back({(absptr_t)voidstar, schema});
        }

        T* dummy = nullptr;
        return fromAnything(schema, (void*)voidstar, dummy);
    }
}


// Hash a value, returning a 16-char hex string
template <typename T>
std::string _mlc_hash(const T& value, Schema* schema) {
    void* voidstar = toAnything(schema, value);
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

// Save a value to file in msgpack format
template <typename T>
void _mlc_save(const T& value, Schema* schema, const std::string& path) {
    void* voidstar = toAnything(schema, value);
    char* errmsg = NULL;
    mlc_save(voidstar, schema, path.c_str(), &errmsg);
    shfree_cpp(voidstar);
    if (errmsg != NULL) {
        PROPAGATE_ERROR(errmsg)
    }
}

// Save a value to file in flat voidstar binary format
template <typename T>
void _mlc_save_voidstar(const T& value, Schema* schema, const std::string& path) {
    void* voidstar = toAnything(schema, value);
    char* errmsg = NULL;
    mlc_save_voidstar(voidstar, schema, path.c_str(), &errmsg);
    shfree_cpp(voidstar);
    if (errmsg != NULL) {
        PROPAGATE_ERROR(errmsg)
    }
}

// Save a value to file in JSON format
template <typename T>
void _mlc_save_json(const T& value, Schema* schema, const std::string& path) {
    void* voidstar = toAnything(schema, value);
    char* errmsg = NULL;
    mlc_save_json(voidstar, schema, path.c_str(), &errmsg);
    shfree_cpp(voidstar);
    if (errmsg != NULL) {
        PROPAGATE_ERROR(errmsg)
    }
}

// Serialize a value to a JSON string
template <typename T>
std::string _mlc_show(const T& value, Schema* schema) {
    void* voidstar = toAnything(schema, value);
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

// Deserialize a JSON string to a typed value
// Returns std::nullopt on parse failure
template <typename T>
std::optional<T> _mlc_read(Schema* schema, const std::string& json_str) {
    char* errmsg = NULL;
    void* voidstar = mlc_read(json_str.c_str(), schema, &errmsg);
    if (errmsg != NULL) {
        PROPAGATE_ERROR(errmsg)
    }
    if (voidstar == NULL) {
        return std::nullopt;
    }
    T* dummy = nullptr;
    T result = fromAnything(schema, voidstar, dummy);
    shfree_cpp(voidstar);
    return result;
}

// Load a value from file, auto-detecting format
// Returns std::nullopt if file does not exist
template <typename T>
std::optional<T> _mlc_load(Schema* schema, const std::string& path) {
    char* errmsg = NULL;
    void* voidstar = mlc_load(path.c_str(), schema, &errmsg);
    if (errmsg != NULL) {
        PROPAGATE_ERROR(errmsg)
    }
    if (voidstar == NULL) {
        return std::nullopt;
    }
    T* dummy = nullptr;
    T result = fromAnything(schema, voidstar, dummy);
    shfree_cpp(voidstar);
    return result;
}

uint8_t* foreign_call(const char* socket_filename, size_t mid, ...) {
    char* errmsg = NULL;
    va_list args;
    size_t nargs = 0;

    char socket_path[128];
    snprintf(socket_path, sizeof(socket_path), "%s/%s", g_tmpdir, socket_filename);

    // Count arguments (must be NULL-terminated)
    va_start(args, mid);
    while (va_arg(args, uint8_t*) != NULL) nargs++;
    va_end(args);

    // Allocate and populate args array
    const uint8_t** args_array = (const uint8_t**)malloc((nargs + 1) * sizeof(uint8_t*));
    if (!args_array) throw std::runtime_error("malloc failed in foreign_call");

    va_start(args, mid);
    for (size_t i = 0; i < nargs; i++) {
        args_array[i] = va_arg(args, uint8_t*);
    }
    args_array[nargs] = NULL;  // Sentinel
    va_end(args);

    // Original logic with variadic args converted to array
    uint8_t* packet = make_morloc_local_call_packet((uint32_t)mid, args_array, nargs, &errmsg);
    if (errmsg != NULL) {
        free(args_array);
        PROPAGATE_ERROR(errmsg)
    }

    pool_mark_busy();
    uint8_t* result = send_and_receive_over_socket(socket_path, packet, &errmsg);
    pool_mark_idle();

    free(packet);

    if (errmsg != NULL) {
        free(args_array);
        PROPAGATE_ERROR(errmsg)
    }

    // Incref the result's SHM so the callee's tracker flush won't destroy
    // data we may still need (e.g. forwarded result packets).
    {
        const morloc_packet_header_t* res_header = (const morloc_packet_header_t*)result;
        if (res_header->command.data.source == PACKET_SOURCE_RPTR) {
            size_t relptr = *(size_t*)(result + res_header->offset + sizeof(morloc_packet_header_t));
            char* resolve_err = NULL;
            void* res_voidstar = rel2abs(relptr, &resolve_err);
            if (resolve_err) { free(resolve_err); resolve_err = NULL; }
            if (res_voidstar) {
                char* incref_err = NULL;
                shincref((absptr_t)res_voidstar, &incref_err);
                if (incref_err) { free(incref_err); }
                _shm_tracker.push_back({(absptr_t)res_voidstar, nullptr});
            }
        }
    }

    free(args_array);
    return result;
}



// AUTO signatures statements start
uint8_t* m1();
uint8_t* m1533();
uint8_t* m1720();
uint8_t* m1730();
uint8_t* m1743();
uint8_t* m1755();
uint8_t* m1810();
uint8_t* m1819();
uint8_t* m1829();
uint8_t* m1839();
uint8_t* m1885();
uint8_t* m1894();
uint8_t* m1904();
uint8_t* m1914();
// AUTO signatures statements end



// AUTO manifolds statements start
std::tuple<int,int> m1567()
{
    try
    {
        uint8_t* s1 = foreign_call("pipe-py", 1567, NULL);
        return(_get_value<std::tuple<int,int>>(s1, mlc_schema_table[0]));
    } catch (...)
      {
          throw;
      }
}

int m1574()
{
    try
    {
        int n2 = std::get<0>(m1567());
        return(n2);
    } catch (...)
      {
          throw;
      }
}

uint8_t* m1()
{
    try
    {
        bool n3 = morloc_eq(0, m1574());
        return(_put_value(n3, mlc_schema_table[1]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1533()
{
    try
    {
        std::tuple<int,int> n1 = std::make_tuple(0, 0);
        return(_put_value(n1, mlc_schema_table[0]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1720()
{
    try
    {
        std::vector<double> n1 = morloc_fill1(0.0, 0);
        return(_put_value(n1, mlc_schema_table[2]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1730()
{
    try
    {
        std::vector<double> n1 = morloc_fill1(7.0, 3);
        return(_put_value(n1, mlc_schema_table[3]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1743()
{
    try
    {
        std::vector<double> n1 = morloc_fill1(-1.5, 4);
        return(_put_value(n1, mlc_schema_table[4]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1755()
{
    try
    {
        std::vector<double> n1 = morloc_fill1(0.5, 8);
        return(_put_value(n1, mlc_schema_table[5]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1810()
{
    try
    {
        std::vector<double> n1 = morloc_ones1(0);
        return(_put_value(n1, mlc_schema_table[2]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1819()
{
    try
    {
        std::vector<double> n1 = morloc_ones1(1);
        return(_put_value(n1, mlc_schema_table[6]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1829()
{
    try
    {
        std::vector<double> n1 = morloc_ones1(3);
        return(_put_value(n1, mlc_schema_table[3]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1839()
{
    try
    {
        std::vector<double> n1 = morloc_ones1(6);
        return(_put_value(n1, mlc_schema_table[7]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1885()
{
    try
    {
        std::vector<double> n1 = morloc_zeros1(0);
        return(_put_value(n1, mlc_schema_table[2]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1894()
{
    try
    {
        std::vector<double> n1 = morloc_zeros1(1);
        return(_put_value(n1, mlc_schema_table[6]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1904()
{
    try
    {
        std::vector<double> n1 = morloc_zeros1(3);
        return(_put_value(n1, mlc_schema_table[3]));
    } catch (...)
      {
          throw;
      }
}
uint8_t* m1914()
{
    try
    {
        std::vector<double> n1 = morloc_zeros1(6);
        return(_put_value(n1, mlc_schema_table[7]));
    } catch (...)
      {
          throw;
      }
}
// AUTO manifolds statements end



// AUTO dispatch start
uint8_t* local_dispatch(uint32_t mid, const uint8_t** args){
    switch(mid){
        case 1: return m1();
        case 1533: return m1533();
        case 1720: return m1720();
        case 1730: return m1730();
        case 1743: return m1743();
        case 1755: return m1755();
        case 1810: return m1810();
        case 1819: return m1819();
        case 1829: return m1829();
        case 1839: return m1839();
        case 1885: return m1885();
        case 1894: return m1894();
        case 1904: return m1904();
        case 1914: return m1914();
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


// Wrappers to adapt compiler-generated dispatch functions to pool_dispatch_fn_t.
// These catch C++ exceptions so the C pool_main never sees them.
static uint8_t* cpp_local_dispatch(uint32_t mid, const uint8_t** args,
                                    size_t nargs, void* ctx) {
    (void)nargs; (void)ctx;
    // Free SHM from previous dispatch (result packet consumed by caller)
    _flush_shm_tracker();
    try {
        return local_dispatch(mid, args);
    } catch (const std::exception& e) {
        return make_fail_packet(e.what());
    } catch (...) {
        return make_fail_packet("An unknown error occurred");
    }
}

static uint8_t* cpp_remote_dispatch(uint32_t mid, const uint8_t** args,
                                     size_t nargs, void* ctx) {
    (void)nargs; (void)ctx;
    try {
        return remote_dispatch(mid, args);
    } catch (const std::exception& e) {
        return make_fail_packet(e.what());
    } catch (...) {
        return make_fail_packet("An unknown error occurred");
    }
}


int main(int argc, char* argv[]) {
    // Line-buffer stderr so diagnostic output is not lost on pool shutdown.
    // stdout is left fully buffered for performance (genome-scale piping)
    // and flushed after each job by pool.c.
    setvbuf(stderr, NULL, _IOLBF, 0);

    // Request SIGTERM when the parent (nexus) dies. Without this,
    // SIGKILL on the nexus leaves pool processes orphaned with
    // leaked SHM segments in /dev/shm.
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif

    // Health check: confirm binary links and print version
    if (argc == 2 && std::string(argv[1]) == "--health") {
        std::cout << "{\"status\":\"ok\",\"version\":\"0.82.0\"}" << std::endl;
        return 0;
    }

    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <socket_path> <tmpdir> <shm_basename>\n";
        return 1;
    }

    g_tmpdir = strdup(argv[2]);

    pool_config_t config = {};
    config.local_dispatch = cpp_local_dispatch;
    config.remote_dispatch = cpp_remote_dispatch;
    config.dispatch_ctx = NULL;
    config.concurrency = POOL_THREADS;
    config.initial_workers = 1;
    config.dynamic_scaling = true;

    _init_schemas();
    int result = pool_main(argc, argv, &config);

    free(g_tmpdir);
    return result;
}
