import signal
import sys
import select
import os # required for setting path to morloc dependencies
import time
import copy
import array
import struct
import socket as _socket
import threading
import queue
from collections import OrderedDict
from multiprocessing import Process, Value, RawValue
import ctypes
import functools
import importlib
import importlib.util


# Global variables for clean signal handling
daemon = None
# The daemon pointer is also held here, and is taken out exactly once by
# whichever of the signal handler and the normal exit path reaches it first.
# `list.pop` is a single C call, so the interpreter cannot process a pending
# signal partway through it: a handler re-entering during cleanup finds the
# slot empty rather than a pointer that is already being freed. Making the
# hand-off atomic is what lets the handler leave signal dispositions alone --
# see 'signal_handler'.
_daemon_slot = []
workers = []
global_state = dict()
_shutdown_wakeup_fd = -1

# The language preamble (runtime bootstrap: `import pymorloc as morloc`, path
# setup) and the generated schema/closure tables run at module top, in the
# coordinator parent -- they are thread-free and the parent depends on them.
# A wire codec is either a schema string (a value) or a closure codec: the
# closure tuple's schema, the codecs of the closure's arguments, and the
# codec of its result. A function whose result is a suspension (a closure
# of no arguments) has a closure codec as its result codec, and so on
# through any depth. Defined ahead of the generated closure table, which
# is built from it.
def mlc_closure_codec(tuple_schema, arg_codecs, res_codec):
    return ("__mlc_closure__", tuple_schema, arg_codecs, res_codec)

# AUTO include preamble start
sys.path = [os.path.expanduser("/opt/morloc/opt"), os.path.expanduser("/opt/morloc-state/src/morloc/plane"), os.path.expanduser("/opt/morloc-state/modules/python")] + sys.path
import importlib
import pymorloc as morloc
_mlc_source_roots = [os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")), os.path.expanduser("."), os.path.expanduser("/opt/morloc-state/src/morloc/plane")]
sys.path = sys.path + _mlc_source_roots[:2]
mlc_schema_table = [ "b"
, "j"
, "i1"
, "i2"
, "i4"
, "i8"
, "u1"
, "u2"
, "u4"
, "u8"
, "f4"
, "f8"
, "<numpy.ndarray>a:3j"
, "<numpy.ndarray>a:3i1"
, "<numpy.ndarray>a:3i2"
, "<numpy.ndarray>a:3i4"
, "<numpy.ndarray>a:3i8"
, "<numpy.ndarray>a:3u1"
, "<numpy.ndarray>a:3u2"
, "<numpy.ndarray>a:3u4"
, "<numpy.ndarray>a:3u8"
, "<numpy.ndarray>a:3f4"
, "<numpy.ndarray>a:3f8"
, "<numpy.ndarray>a:3b"
, "<numpy.ndarray>a:3s"
, "<numpy.ndarray>a:2t2js"
, "<numpy.ndarray>a:0j"
, "<numpy.ndarray>a:0f8"
, "<numpy.ndarray>a:4f8"
, "<numpy.ndarray>a:1f8"
, "<numpy.ndarray>a:6f8"
, "t2jj" ]

# AUTO include preamble end

# User `source` imports, by contrast, are deferred out of the parent and run in
# each worker AFTER fork (see _mlc_load_user_sources). Importing heavy libraries
# in the parent and then forking is unsafe on macOS: e.g. numpy loads Apple's
# Accelerate/libdispatch, which spins background threads, and forking a
# multithreaded process then deadlocks/aborts in the child (fork-after-threads).
# Deferring keeps the parent single-threaded, and each worker initializes its
# libraries in its own process -- preserving full per-worker thread parallelism.
# The block is held verbatim in a raw string and exec'd into module globals (so
# the imports land at module scope exactly as if run at import time, past fork).
_mlc_user_sources = r'''
# AUTO include user-sources start
__morloc_src_default_vector_test_vector_test = _mlc_import_source("default.vector.test.vector-test")
# AUTO include user-sources end
'''

_mlc_sources_loaded = False
_mlc_source_error = None

def _mlc_import_source(module_path):
    # Load a `source`d file by location rather than by module name.
    #
    # A user file may be named after a module that is already in sys.modules --
    # `copy` and `time` are imported above, and they pull in others such as
    # `heapq` transitively -- and importing by name returns that module instead
    # of the user's file, so none of their functions are found. The set is not
    # one a user can be expected to know, so resolve the file under the source
    # roots (the program's own, the working directory, the morloc module plane)
    # and load it from there.
    #
    # The module is registered under a reserved key, so it neither reads nor
    # replaces a real module of the same name: a plain `import copy` from inside
    # a user file still reaches the standard library.
    rel = module_path.replace(".", os.sep) + ".py"
    for root in _mlc_source_roots:
        candidate = os.path.join(root, rel)
        if os.path.isfile(candidate):
            key = "_mlc_src_" + module_path.replace(".", "_")
            spec = importlib.util.spec_from_file_location(key, candidate)
            module = importlib.util.module_from_spec(spec)
            sys.modules[key] = module
            spec.loader.exec_module(module)
            return module
    # Nothing on the search path: an installed package, imported by name.
    return importlib.import_module(module_path)


def _mlc_load_user_sources():
    # Idempotent; called once per worker after shinit (and by --health). Any
    # failure is captured in _mlc_source_error so run_job can return it as a
    # clean fail packet rather than dying and masquerading as "connection closed".
    global _mlc_sources_loaded, _mlc_source_error
    if _mlc_sources_loaded:
        return
    # Import-time stdout (package banners/prints) must not corrupt the data
    # stream shared with the nexus; route it to stderr for the duration.
    _saved_stdout = sys.stdout
    sys.stdout = sys.stderr
    try:
        exec(compile(_mlc_user_sources, "<morloc-sources>", "exec"), globals())
        _mlc_sources_loaded = True
    except BaseException as e:
        import traceback
        _mlc_source_error = f"failed to load pool sources: {e!s}\n{traceback.format_exc()}"
        # Surface the failure to stderr immediately at load time. The pool stays
        # up and returns this as a fail packet per call (the deferred-loading
        # contract), and the readiness ping still succeeds because the process
        # and its IPC are alive -- but without this print the cause would be
        # invisible until the first call. Full import validation is --health's
        # job (it exits non-zero on this same error).
        print(_mlc_source_error, file=sys.stderr)
        sys.stderr.flush()
    finally:
        sys.stdout = _saved_stdout

# Dynamic worker spawning: monkey-patch foreign_call to track busy workers.
# Workers atomically increment busy_count before a foreign_call and decrement
# after. When busy_count reaches total_workers, a byte is written to a wake-up
# pipe to tell the main process to spawn a new worker.
_original_foreign_call = morloc.foreign_call
_busy_ref = None
_total_ref = None
_wakeup_fd = -1

def _init_worker_tracking(busy, total, wakeup_fd):
    global _busy_ref, _total_ref, _wakeup_fd
    _busy_ref = busy
    _total_ref = total
    _wakeup_fd = wakeup_fd
    morloc.foreign_call = _tracked_foreign_call

def _tracked_foreign_call(*args):
    prev = _busy_ref.value
    _busy_ref.value = prev + 1
    if prev + 1 >= _total_ref.value and _wakeup_fd >= 0:
        try:
            os.write(_wakeup_fd, b'!')
        except OSError:
            pass
    try:
        return _original_foreign_call(*args)
    finally:
        _busy_ref.value -= 1

def __mlc_wrap_log(group, start_tmpl, pass_tmpl, fail_tmpl, bench_key, fn):
    # bench_key is "group\tname\tlang" when the label carries
    # `benchmark: true`, else None. Only the success path records: a call that
    # raised did not do the work being measured.
    def go(*args):
        call_id = morloc.log_next_id()
        t0 = time.monotonic()
        if start_tmpl is not None:
            morloc.log_emit(start_tmpl, group, 0.0, call_id)
        try:
            r = fn(*args)
            dt = time.monotonic() - t0
            if pass_tmpl is not None:
                morloc.log_emit(pass_tmpl, group, dt, call_id)
            if bench_key is not None:
                morloc.bench_record(bench_key, dt)
            return r
        except BaseException:
            if fail_tmpl is not None:
                morloc.log_emit(fail_tmpl, group, time.monotonic() - t0, call_id)
            raise
    return go


# Defunctionalized-closure support. A morloc function value is a
# functools.partial over a manifold function m<mid>; when it crosses a language
# boundary it travels as the wire tuple (home_language, manifold_id,
# captured_packets) and is applied on the far side by calling back to this pool.

MLC_HOME_LANG = "py"


def _mlc_is_closure_codec(codec):
    return isinstance(codec, tuple) and len(codec) == 4 and codec[0] == "__mlc_closure__"


def mlc_encode(value, codec):
    # Serialize a native value by its codec: a closure is reified first.
    if _mlc_is_closure_codec(codec):
        return morloc.put_value(mlc_reify(value, MLC_HOME_LANG), codec[1])
    return morloc.put_value(value, codec)


def mlc_decode(pkt, codec):
    # Deserialize a packet by its codec: a closure is reflected into a
    # callable that calls back to its home pool.
    if _mlc_is_closure_codec(codec):
        return mlc_reflect_from_tuple(morloc.get_value(pkt, codec[1]), codec[2], codec[3])
    return morloc.get_value(pkt, codec)


# A table arrives as a pyarrow.RecordBatch over shared memory. When the
# module maps Table to another library's type, convert here; the known
# libraries take the batch through the Arrow PyCapsule interface without a
# copy where they can. Any other name is called as module.attr(batch).
def mlc_table_import(batch, typename):
    if typename in ("arrow", "pyarrow.RecordBatch"):
        return batch
    if typename == "pyarrow.Table":
        import pyarrow
        return pyarrow.Table.from_batches([batch])
    if typename == "polars.DataFrame":
        import polars
        return polars.from_arrow(batch)
    if typename == "pandas.DataFrame":
        return batch.to_pandas()
    if typename == "duckdb.DuckDBPyRelation":
        import duckdb
        return duckdb.from_arrow(batch)
    modname, _, attr = typename.rpartition(".")
    if not modname:
        raise TypeError("cannot import a table as %r: not a dotted type name" % typename)
    return getattr(importlib.import_module(modname), attr)(batch)


def mlc_reify(f, home_lang):
    # Recover (home, mid, captured) from a closure and serialize its captured
    # values. A closure built here is a functools.partial over a manifold
    # function m<mid>, with the captured values in the manifold's
    # context-argument order; mlc_closure_table[mid] holds their codecs. A
    # closure reflected from another pool carries its origin and is passed
    # back as it came.
    origin = getattr(f, "__mlc_origin__", None)
    if origin is not None:
        return origin
    if isinstance(f, functools.partial):
        mid = _mlc_manifold_id(f.func)
        captured = list(f.args)
    else:
        # A manifold function itself: a function value with nothing captured.
        mid = _mlc_manifold_id(f)
        captured = []
    # The codecs say what this manifold captures. A missing entry, or a
    # count that disagrees, would otherwise serialize the wrong captures
    # (a zip truncates silently), so the far side would call back with
    # arguments the closure never had.
    if mid not in mlc_closure_table:
        raise RuntimeError(
            f"morloc: no closure codecs for manifold {mid}; it was not "
            "registered as a crossing closure")
    cap_codecs = mlc_closure_table[mid]
    if len(captured) != len(cap_codecs):
        raise RuntimeError(
            f"morloc: manifold {mid} captured {len(captured)} values but "
            f"{len(cap_codecs)} codecs are registered")
    packets = [mlc_encode(c, s) for c, s in zip(captured, cap_codecs)]
    return (home_lang, mid, packets)


def _mlc_manifold_id(f):
    # A morloc-made callable is a generated manifold named m<mid>. Anything
    # else -- a callable the host created, or one morloc received and did
    # not record an origin for -- cannot be reified: the far side has no
    # manifold to call back into. Mirrors the C++ pool's
    # "cannot reify a non-morloc closure".
    name = getattr(f, "__name__", None)
    if name is not None and name.startswith("m") and name[1:].isdigit():
        return int(name[1:])
    raise RuntimeError(
        "morloc: cannot reify a callable that morloc did not create "
        f"({name!r}); a function value made by host code has no identity "
        "to send across a pool boundary")


def mlc_reflect_from_tuple(tup, arg_codecs, res_codec):
    # Rebuild a callable from an already-deserialized closure wire tuple
    # (home_lang, mid, captured_packets). On application it serializes its
    # arguments, appends them to the captured packets, calls back to the
    # producing pool via foreign_call on the closure's manifold id, and
    # decodes the result. Used when the closure is nested in an aggregate
    # whose enclosing get_value has already parsed the tuple.
    home_lang, mid, captured = tup
    sock = os.path.join(global_state["tmpdir"], "pipe-" + home_lang)
    def _call(*args):
        packets = list(captured) + [mlc_encode(a, s) for a, s in zip(args, arg_codecs)]
        return mlc_decode(morloc.foreign_call(sock, mid, packets), res_codec)
    _call.__mlc_origin__ = (home_lang, mid, list(captured))
    return _call


def mlc_reflect(pkt, tuple_schema, arg_codecs, res_codec):
    # Rebuild a callable from a raw incoming closure wire packet: deserialize the
    # tuple, then reflect it. Used when the closure is the top-level crossing
    # value (the whole packet is the closure tuple).
    return mlc_reflect_from_tuple(morloc.get_value(pkt, tuple_schema), arg_codecs, res_codec)


def mlc_make_closure_dispatch(mid, arg_codecs, res_codec):
    # Serial dispatch wrapper for a closure manifold: decode the incoming
    # captured ++ bound argument packets, call the native manifold, and
    # encode the result. Registered under the closure's mid so a foreign
    # apply reaches it.
    fn = globals()["m" + str(mid)]
    def _wrapper(*sargs):
        args = [mlc_decode(s, sch) for s, sch in zip(sargs, arg_codecs)]
        return mlc_encode(fn(*args), res_codec)
    return _wrapper


# AUTO include manifolds start
def m3490():
    try:
        s60 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3490
        , [] )
        return(morloc.get_value(s60, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3490, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:214:73)")

def m3508():
    try:
        s61 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3508
        , [] )
        return(morloc.get_value(s61, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3508, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:215:73)")

def m3526():
    try:
        s62 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3526
        , [] )
        return(morloc.get_value(s62, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3526, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:216:69)")

def m3544():
    try:
        s63 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3544
        , [] )
        return(morloc.get_value(s63, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3544, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:217:66)")

def m3560():
    try:
        s64 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3560
        , [] )
        return(morloc.get_value(s64, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3560, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:218:73)")

def m3576():
    try:
        s65 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3576
        , [] )
        return(morloc.get_value(s65, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3576, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:219:73)")

def m3592():
    try:
        s66 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3592
        , [] )
        return(morloc.get_value(s66, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3592, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:220:74)")

def m3608():
    try:
        s67 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3608
        , [] )
        return(morloc.get_value(s67, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3608, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:221:74)")

def m3624():
    try:
        s68 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3624
        , [] )
        return(morloc.get_value(s68, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3624, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:222:73)")

def m3636():
    try:
        s69 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3636
        , [] )
        return(morloc.get_value(s69, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3636, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:223:73)")

def m3652():
    try:
        s70 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3652
        , [] )
        return(morloc.get_value(s70, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3652, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:224:73)")

def m3668():
    try:
        s71 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3668
        , [] )
        return(morloc.get_value(s71, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3668, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:225:88)")

def m3690():
    try:
        s72 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3690
        , [] )
        return(morloc.get_value(s72, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3690, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:226:88)")

def m3869():
    try:
        s73 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3869
        , [] )
        return(morloc.get_value(s73, mlc_schema_table[1]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3869, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:188:31)")

def m3885():
    try:
        s74 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3885
        , [] )
        return(morloc.get_value(s74, mlc_schema_table[2]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3885, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:189:30)")

def m3901():
    try:
        s75 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3901
        , [] )
        return(morloc.get_value(s75, mlc_schema_table[3]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3901, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:190:31)")

def m3917():
    try:
        s76 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3917
        , [] )
        return(morloc.get_value(s76, mlc_schema_table[4]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3917, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:191:31)")

def m3933():
    try:
        s77 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3933
        , [] )
        return(morloc.get_value(s77, mlc_schema_table[5]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3933, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:192:31)")

def m3949():
    try:
        s78 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3949
        , [] )
        return(morloc.get_value(s78, mlc_schema_table[6]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3949, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:193:30)")

def m3965():
    try:
        s79 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3965
        , [] )
        return(morloc.get_value(s79, mlc_schema_table[7]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3965, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:194:31)")

def m3981():
    try:
        s80 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3981
        , [] )
        return(morloc.get_value(s80, mlc_schema_table[8]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3981, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:195:31)")

def m3997():
    try:
        s81 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3997
        , [] )
        return(morloc.get_value(s81, mlc_schema_table[9]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3997, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:196:31)")

def m4013():
    try:
        s82 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4013
        , [] )
        return(morloc.get_value(s82, mlc_schema_table[10]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4013, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:197:31)")

def m4029():
    try:
        s83 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4029
        , [] )
        return(morloc.get_value(s83, mlc_schema_table[11]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4029, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:198:31)")

def m4045():
    try:
        s84 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4045
        , [] )
        return(morloc.get_value(s84, mlc_schema_table[11]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4045, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:199:32)")

def m4059():
    try:
        s85 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4059
        , [] )
        return(morloc.get_value(s85, mlc_schema_table[1]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4059, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:200:33)")

def m4161():
    try:
        s86 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4161
        , [] )
        return(morloc.get_value(s86, mlc_schema_table[12]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4161, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:26)")

def m4172():
    try:
        s87 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4172
        , [] )
        return(morloc.get_value(s87, mlc_schema_table[12]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4172, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:164:66)")

def m4183():
    try:
        s88 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4183
        , [] )
        return(morloc.get_value(s88, mlc_schema_table[13]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4183, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:25)")

def m4194():
    try:
        s89 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4194
        , [] )
        return(morloc.get_value(s89, mlc_schema_table[13]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4194, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:165:64)")

def m4205():
    try:
        s90 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4205
        , [] )
        return(morloc.get_value(s90, mlc_schema_table[14]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4205, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:26)")

def m4216():
    try:
        s91 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4216
        , [] )
        return(morloc.get_value(s91, mlc_schema_table[14]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4216, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:166:66)")

def m4227():
    try:
        s92 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4227
        , [] )
        return(morloc.get_value(s92, mlc_schema_table[15]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4227, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:26)")

def m4238():
    try:
        s93 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4238
        , [] )
        return(morloc.get_value(s93, mlc_schema_table[15]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4238, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:167:66)")

def m4249():
    try:
        s94 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4249
        , [] )
        return(morloc.get_value(s94, mlc_schema_table[16]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4249, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:26)")

def m4260():
    try:
        s95 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4260
        , [] )
        return(morloc.get_value(s95, mlc_schema_table[16]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4260, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:168:66)")

def m4271():
    try:
        s96 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4271
        , [] )
        return(morloc.get_value(s96, mlc_schema_table[17]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4271, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:25)")

def m4282():
    try:
        s97 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4282
        , [] )
        return(morloc.get_value(s97, mlc_schema_table[17]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4282, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:169:64)")

def m4293():
    try:
        s98 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4293
        , [] )
        return(morloc.get_value(s98, mlc_schema_table[18]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4293, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:26)")

def m4304():
    try:
        s99 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4304
        , [] )
        return(morloc.get_value(s99, mlc_schema_table[18]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4304, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:170:66)")

def m4315():
    try:
        s100 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4315
        , [] )
        return(morloc.get_value(s100, mlc_schema_table[19]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4315, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:26)")

def m4326():
    try:
        s101 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4326
        , [] )
        return(morloc.get_value(s101, mlc_schema_table[19]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4326, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:171:66)")

def m4337():
    try:
        s102 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4337
        , [] )
        return(morloc.get_value(s102, mlc_schema_table[20]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4337, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:26)")

def m4348():
    try:
        s103 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4348
        , [] )
        return(morloc.get_value(s103, mlc_schema_table[20]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4348, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:172:66)")

def m4359():
    try:
        s104 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4359
        , [] )
        return(morloc.get_value(s104, mlc_schema_table[21]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4359, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:26)")

def m4370():
    try:
        s105 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4370
        , [] )
        return(morloc.get_value(s105, mlc_schema_table[21]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4370, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:173:74)")

def m4381():
    try:
        s106 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4381
        , [] )
        return(morloc.get_value(s106, mlc_schema_table[22]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4381, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:26)")

def m4392():
    try:
        s107 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4392
        , [] )
        return(morloc.get_value(s107, mlc_schema_table[22]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4392, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:174:74)")

def m4403():
    try:
        s108 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4403
        , [] )
        return(morloc.get_value(s108, mlc_schema_table[22]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4403, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:27)")

def m4414():
    try:
        s109 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4414
        , [] )
        return(morloc.get_value(s109, mlc_schema_table[22]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4414, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:175:76)")

def m4425():
    try:
        s110 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4425
        , [] )
        return(morloc.get_value(s110, mlc_schema_table[23]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4425, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:30)")

def m4432():
    try:
        s111 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4432
        , [] )
        return(morloc.get_value(s111, mlc_schema_table[23]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4432, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:176:78)")

def m4443():
    try:
        s112 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4443
        , [] )
        return(morloc.get_value(s112, mlc_schema_table[24]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4443, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:29)")

def m4450():
    try:
        s113 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4450
        , [] )
        return(morloc.get_value(s113, mlc_schema_table[24]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4450, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:177:72)")

def m4461():
    try:
        s114 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4461
        , [] )
        return(morloc.get_value(s114, mlc_schema_table[25]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4461, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:35)")

def m4471():
    try:
        s115 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4471
        , [] )
        return(morloc.get_value(s115, mlc_schema_table[25]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4471, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:178:90)")

def m4483():
    try:
        s116 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4483
        , [] )
        return(morloc.get_value(s116, mlc_schema_table[26]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4483, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:28)")

def m4491():
    try:
        s117 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4491
        , [] )
        return(morloc.get_value(s117, mlc_schema_table[26]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4491, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:179:61)")

def m4694():
    try:
        s118 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4694
        , [] )
        return(morloc.get_value(s118, mlc_schema_table[27]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4694, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:149:31)")

def m4698():
    try:
        s119 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4698
        , [] )
        return(morloc.get_value(s119, mlc_schema_table[27]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4698, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:149:65)")

def m4706():
    try:
        s120 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4706
        , [] )
        return(morloc.get_value(s120, mlc_schema_table[22]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4706, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:150:31)")

def m4710():
    try:
        s121 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4710
        , [] )
        return(morloc.get_value(s121, mlc_schema_table[22]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4710, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:150:65)")

def m4719():
    try:
        s122 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4719
        , [] )
        return(morloc.get_value(s122, mlc_schema_table[28]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4719, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:151:31)")

def m4723():
    try:
        s123 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4723
        , [] )
        return(morloc.get_value(s123, mlc_schema_table[28]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4723, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:151:65)")

def m4781():
    try:
        s124 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4781
        , [] )
        return(morloc.get_value(s124, mlc_schema_table[27]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4781, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:141:26)")

def m4784():
    try:
        s125 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4784
        , [] )
        return(morloc.get_value(s125, mlc_schema_table[27]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4784, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:141:53)")

def m4792():
    try:
        s126 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4792
        , [] )
        return(morloc.get_value(s126, mlc_schema_table[29]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4792, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:142:26)")

def m4795():
    try:
        s127 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4795
        , [] )
        return(morloc.get_value(s127, mlc_schema_table[29]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4795, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:142:53)")

def m4804():
    try:
        s128 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4804
        , [] )
        return(morloc.get_value(s128, mlc_schema_table[22]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4804, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:143:26)")

def m4807():
    try:
        s129 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4807
        , [] )
        return(morloc.get_value(s129, mlc_schema_table[22]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4807, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:143:53)")

def m4816():
    try:
        s130 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4816
        , [] )
        return(morloc.get_value(s130, mlc_schema_table[30]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4816, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:144:26)")

def m4819():
    try:
        s131 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4819
        , [] )
        return(morloc.get_value(s131, mlc_schema_table[30]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4819, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:144:53)")

def m4890():
    try:
        s132 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4890
        , [] )
        return(morloc.get_value(s132, mlc_schema_table[27]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4890, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:133:27)")

def m4893():
    try:
        s133 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4893
        , [] )
        return(morloc.get_value(s133, mlc_schema_table[27]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4893, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:133:55)")

def m4901():
    try:
        s134 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4901
        , [] )
        return(morloc.get_value(s134, mlc_schema_table[29]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4901, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:134:27)")

def m4904():
    try:
        s135 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4904
        , [] )
        return(morloc.get_value(s135, mlc_schema_table[29]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4904, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:134:55)")

def m4913():
    try:
        s136 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4913
        , [] )
        return(morloc.get_value(s136, mlc_schema_table[22]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4913, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:135:27)")

def m4916():
    try:
        s137 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4916
        , [] )
        return(morloc.get_value(s137, mlc_schema_table[22]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4916, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:135:55)")

def m4925():
    try:
        s138 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4925
        , [] )
        return(morloc.get_value(s138, mlc_schema_table[30]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4925, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:136:27)")

def m4928():
    try:
        s139 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4928
        , [] )
        return(morloc.get_value(s139, mlc_schema_table[30]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4928, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:136:55)")

def m5014(s140):
    try:
        s142 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5014
        , [s140] )
        return(morloc.get_value(s142, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5014, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:94:43)")

def m5026(s140):
    try:
        s143 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5026
        , [s140] )
        return(morloc.get_value(s143, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5026, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:95:45)")

def m5038(s140):
    try:
        s144 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5038
        , [s140] )
        return(morloc.get_value(s144, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5038, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:96:45)")

def m5050(s140):
    try:
        s145 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5050
        , [s140] )
        return(morloc.get_value(s145, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5050, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:97:47)")

def m5065(s140):
    try:
        s146 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5065
        , [s140] )
        return(morloc.get_value(s146, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5065, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:98:45)")

def m5079(s140):
    try:
        s147 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5079
        , [s140] )
        return(morloc.get_value(s147, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5079, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:99:48)")

def m5093(s140):
    try:
        s148 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5093
        , [s140] )
        return(morloc.get_value(s148, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5093, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:100:48)")

def m5107(s140):
    try:
        s149 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5107
        , [s140] )
        return(morloc.get_value(s149, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5107, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:101:48)")

def m5121(s140):
    try:
        s150 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5121
        , [s140] )
        return(morloc.get_value(s150, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5121, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:102:53)")

def m5137(s140):
    try:
        s151 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5137
        , [s140] )
        return(morloc.get_value(s151, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5137, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:103:55)")

def m5154(s140):
    try:
        s152 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5154
        , [s140] )
        return(morloc.get_value(s152, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5154, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:104:51)")

def m5169(s140):
    try:
        s153 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5169
        , [s140] )
        return(morloc.get_value(s153, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5169, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:105:52)")

def m5183(s140):
    try:
        s154 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5183
        , [s140] )
        return(morloc.get_value(s154, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5183, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:106:50)")

def m5197(s140):
    try:
        s155 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5197
        , [s140] )
        return(morloc.get_value(s155, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5197, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:107:49)")

def m5212(s140):
    try:
        s156 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5212
        , [s140] )
        return(morloc.get_value(s156, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5212, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:108:60)")

def m5231(s140):
    try:
        s157 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5231
        , [s140] )
        return(morloc.get_value(s157, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5231, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:109:67)")

def m5246(s140):
    try:
        s158 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5246
        , [s140] )
        return(morloc.get_value(s158, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5246, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:110:47)")

def m5260(s140):
    try:
        s159 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5260
        , [s140] )
        return(morloc.get_value(s159, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5260, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:111:57)")

def m5279(s141):
    try:
        s160 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5279
        , [s141] )
        return(morloc.get_value(s160, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5279, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:112:59)")

def m5297(s140):
    try:
        s161 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5297
        , [s140] )
        return(morloc.get_value(s161, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5297, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:113:59)")

def m5316(s140):
    try:
        s162 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5316
        , [s140] )
        return(morloc.get_value(s162, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5316, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:114:53)")

def m5333(s140):
    try:
        s163 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5333
        , [s140] )
        return(morloc.get_value(s163, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5333, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:115:53)")

def m5350(s140):
    try:
        s164 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5350
        , [s140] )
        return(morloc.get_value(s164, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5350, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:116:55)")

def m5367(s140):
    try:
        s165 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5367
        , [s140] )
        return(morloc.get_value(s165, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5367, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:117:55)")

def m5384(s140):
    try:
        s166 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5384
        , [s140] )
        return(morloc.get_value(s166, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5384, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:118:55)")

def m5401(s140):
    try:
        s167 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5401
        , [s140] )
        return(morloc.get_value(s167, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5401, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:119:57)")

def m5418(s140):
    try:
        s168 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5418
        , [s140] )
        return(morloc.get_value(s168, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5418, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:120:57)")

def m5433(s140):
    try:
        s169 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5433
        , [s140] )
        return(morloc.get_value(s169, mlc_schema_table[0]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5433, /opt/morloc-state/src/morloc/plane/default/vector/test/main.loc:121:59)")

def m4985():
    try:
        s170 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 3397
        , [] )
        n171 = __morloc_src_default_vector_test_vector_test.printMsg( "Slicer"
        , morloc.get_value(s170, mlc_schema_table[31]) )
        return(n171)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4985, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5614(s140):
    try:
        n172 = __morloc_src_default_vector_test_vector_test.testTrue( ".[7:2:-2] ten == [7,5,3]"
        , m5433(s140)
        , m4985() )
        return(n172)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5614, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5608(s140):
    try:
        n173 = __morloc_src_default_vector_test_vector_test.testTrue( ".[2:7:2] ten == [2,4,6]"
        , m5418(s140)
        , m5614(s140) )
        return(n173)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5608, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5602(s140):
    try:
        n174 = __morloc_src_default_vector_test_vector_test.testTrue( ".[0:3:1] ten == [0,1,2]"
        , m5401(s140)
        , m5608(s140) )
        return(n174)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5602, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5596(s140):
    try:
        n175 = __morloc_src_default_vector_test_vector_test.testTrue( ".[:3:1] ten == [0,1,2]"
        , m5384(s140)
        , m5602(s140) )
        return(n175)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5596, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5590(s140):
    try:
        n176 = __morloc_src_default_vector_test_vector_test.testTrue( ".[7::1] ten == [7,8,9]"
        , m5367(s140)
        , m5596(s140) )
        return(n176)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5590, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5584(s140):
    try:
        n177 = __morloc_src_default_vector_test_vector_test.testTrue( ".[0:3:] ten == [0,1,2]"
        , m5350(s140)
        , m5590(s140) )
        return(n177)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5584, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5578(s140):
    try:
        n178 = __morloc_src_default_vector_test_vector_test.testTrue( ".[:3:] ten == [0,1,2]"
        , m5333(s140)
        , m5584(s140) )
        return(n178)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5578, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5572(s140):
    try:
        n179 = __morloc_src_default_vector_test_vector_test.testTrue( ".[7::] ten == [7,8,9]"
        , m5316(s140)
        , m5578(s140) )
        return(n179)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5572, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5566(s140):
    try:
        n180 = __morloc_src_default_vector_test_vector_test.testTrue( ".[::-2] ten == [9,7,5,3,1]"
        , m5297(s140)
        , m5572(s140) )
        return(n180)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5566, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5560(s141, s140):
    try:
        n181 = __morloc_src_default_vector_test_vector_test.testTrue( ".[::-1] four == [3,2,1,0]"
        , m5279(s141)
        , m5566(s140) )
        return(n181)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5560, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5554(s141, s140):
    try:
        n182 = __morloc_src_default_vector_test_vector_test.testTrue( ".[::2] ten == [0,2,4,6,8]"
        , m5260(s140)
        , m5560(s141, s140) )
        return(n182)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5554, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5548(s141, s140):
    try:
        n183 = __morloc_src_default_vector_test_vector_test.testTrue( ".[::] ten == ten"
        , m5246(s140)
        , m5554(s141, s140) )
        return(n183)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5548, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5542(s141, s140):
    try:
        n184 = __morloc_src_default_vector_test_vector_test.testTrue( ".[(1 :: Int):2] ten == [1]"
        , m5231(s140)
        , m5548(s141, s140) )
        return(n184)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5542, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5536(s141, s140):
    try:
        n185 = __morloc_src_default_vector_test_vector_test.testTrue( ".[-2:-1] ten == .[8:9] ten"
        , m5212(s140)
        , m5542(s141, s140) )
        return(n185)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5536, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5530(s141, s140):
    try:
        n186 = __morloc_src_default_vector_test_vector_test.testTrue( ".[-1:] ten == [9]"
        , m5197(s140)
        , m5536(s141, s140) )
        return(n186)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5530, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5524(s141, s140):
    try:
        n187 = __morloc_src_default_vector_test_vector_test.testTrue( ".[-1:0] ten == []"
        , m5183(s140)
        , m5530(s141, s140) )
        return(n187)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5524, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5518(s141, s140):
    try:
        n188 = __morloc_src_default_vector_test_vector_test.testTrue( ".[0:-10] ten == []"
        , m5169(s140)
        , m5524(s141, s140) )
        return(n188)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5518, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5512(s141, s140):
    try:
        n189 = __morloc_src_default_vector_test_vector_test.testTrue( ".[0:-9] ten == [0]"
        , m5154(s140)
        , m5518(s141, s140) )
        return(n189)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5512, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5506(s141, s140):
    try:
        n190 = __morloc_src_default_vector_test_vector_test.testTrue( ".[6:-1] ten == [6,7,8]"
        , m5137(s140)
        , m5512(s141, s140) )
        return(n190)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5506, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5500(s141, s140):
    try:
        n191 = __morloc_src_default_vector_test_vector_test.testTrue( ".[8:99] ten == [8,9]"
        , m5121(s140)
        , m5506(s141, s140) )
        return(n191)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5500, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5494(s141, s140):
    try:
        n192 = __morloc_src_default_vector_test_vector_test.testTrue( ".[1:0] ten == []"
        , m5107(s140)
        , m5500(s141, s140) )
        return(n192)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5494, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5488(s141, s140):
    try:
        n193 = __morloc_src_default_vector_test_vector_test.testTrue( ".[1:1] ten == []"
        , m5093(s140)
        , m5494(s141, s140) )
        return(n193)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5488, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5482(s141, s140):
    try:
        n194 = __morloc_src_default_vector_test_vector_test.testTrue( ".[0:0] ten == []"
        , m5079(s140)
        , m5488(s141, s140) )
        return(n194)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5482, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5476(s141, s140):
    try:
        n195 = __morloc_src_default_vector_test_vector_test.testTrue( ".[:] ten == ten"
        , m5065(s140)
        , m5482(s141, s140) )
        return(n195)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5476, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5470(s141, s140):
    try:
        n196 = __morloc_src_default_vector_test_vector_test.testTrue( ".[1+1] ten == 2"
        , m5050(s140)
        , m5476(s141, s140) )
        return(n196)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5470, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5464(s141, s140):
    try:
        n197 = __morloc_src_default_vector_test_vector_test.testTrue( ".[-2] ten == 8"
        , m5038(s140)
        , m5470(s141, s140) )
        return(n197)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5464, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m5458(s141, s140):
    try:
        n198 = __morloc_src_default_vector_test_vector_test.testTrue( ".[-1] ten == 9"
        , m5026(s140)
        , m5464(s141, s140) )
        return(n198)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=5458, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4869(s141, s140):
    try:
        n199 = __morloc_src_default_vector_test_vector_test.testTrue( ".[0] ten == 0"
        , m5014(s140)
        , m5458(s141, s140) )
        return(n199)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4869, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4882():
    try:
        s140 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 4989
        , [] )
        s141 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 5002
        , [] )
        n200 = m4869(s141, s140)
        n201 = __morloc_src_default_vector_test_vector_test.printMsg( "Zeros"
        , n200 )
        return(n201)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4882, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4962():
    try:
        n202 = __morloc_src_default_vector_test_vector_test.testEqual( "zeros1 6"
        , m4925()
        , m4928()
        , m4882() )
        return(n202)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4962, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4951():
    try:
        n203 = __morloc_src_default_vector_test_vector_test.testEqual( "zeros1 3"
        , m4913()
        , m4916()
        , m4962() )
        return(n203)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4951, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4940():
    try:
        n204 = __morloc_src_default_vector_test_vector_test.testEqual( "zeros1 1"
        , m4901()
        , m4904()
        , m4951() )
        return(n204)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4940, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4760():
    try:
        n205 = __morloc_src_default_vector_test_vector_test.testEqual( "zeros1 0"
        , m4890()
        , m4893()
        , m4940() )
        return(n205)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4760, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4773():
    try:
        n206 = __morloc_src_default_vector_test_vector_test.printMsg( "Ones"
        , m4760() )
        return(n206)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4773, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4853():
    try:
        n207 = __morloc_src_default_vector_test_vector_test.testEqual( "ones1 6"
        , m4816()
        , m4819()
        , m4773() )
        return(n207)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4853, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4842():
    try:
        n208 = __morloc_src_default_vector_test_vector_test.testEqual( "ones1 3"
        , m4804()
        , m4807()
        , m4853() )
        return(n208)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4842, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4831():
    try:
        n209 = __morloc_src_default_vector_test_vector_test.testEqual( "ones1 1"
        , m4792()
        , m4795()
        , m4842() )
        return(n209)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4831, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4673():
    try:
        n210 = __morloc_src_default_vector_test_vector_test.testEqual( "ones1 0"
        , m4781()
        , m4784()
        , m4831() )
        return(n210)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4673, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4686():
    try:
        n211 = __morloc_src_default_vector_test_vector_test.printMsg( "Fill"
        , m4673() )
        return(n211)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4686, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4744():
    try:
        n212 = __morloc_src_default_vector_test_vector_test.testEqual( "fill1 -1.5 4"
        , m4719()
        , m4723()
        , m4686() )
        return(n212)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4744, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4733():
    try:
        n213 = __morloc_src_default_vector_test_vector_test.testEqual( "fill1 7.0 3"
        , m4706()
        , m4710()
        , m4744() )
        return(n213)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4733, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4140():
    try:
        n214 = __morloc_src_default_vector_test_vector_test.testEqual( "fill1 0.0 0"
        , m4694()
        , m4698()
        , m4733() )
        return(n214)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4140, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4153():
    try:
        n215 = __morloc_src_default_vector_test_vector_test.printMsg( "Functor (map)"
        , m4140() )
        return(n215)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4153, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4655():
    try:
        n216 = __morloc_src_default_vector_test_vector_test.testEqual( "map empty"
        , m4483()
        , m4491()
        , m4153() )
        return(n216)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4655, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4642():
    try:
        n217 = __morloc_src_default_vector_test_vector_test.testEqual( "map (Int,Str) id"
        , m4461()
        , m4471()
        , m4655() )
        return(n217)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4642, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4629():
    try:
        n218 = __morloc_src_default_vector_test_vector_test.testEqual( "map Str id"
        , m4443()
        , m4450()
        , m4642() )
        return(n218)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4629, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4618():
    try:
        n219 = __morloc_src_default_vector_test_vector_test.testEqual( "map Bool id"
        , m4425()
        , m4432()
        , m4629() )
        return(n219)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4618, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4607():
    try:
        n220 = __morloc_src_default_vector_test_vector_test.testEqual( "map Real"
        , m4403()
        , m4414()
        , m4618() )
        return(n220)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4607, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4596():
    try:
        n221 = __morloc_src_default_vector_test_vector_test.testEqual( "map F64"
        , m4381()
        , m4392()
        , m4607() )
        return(n221)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4596, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4585():
    try:
        n222 = __morloc_src_default_vector_test_vector_test.testEqual( "map F32"
        , m4359()
        , m4370()
        , m4596() )
        return(n222)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4585, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4574():
    try:
        n223 = __morloc_src_default_vector_test_vector_test.testEqual( "map U64"
        , m4337()
        , m4348()
        , m4585() )
        return(n223)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4574, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4563():
    try:
        n224 = __morloc_src_default_vector_test_vector_test.testEqual( "map U32"
        , m4315()
        , m4326()
        , m4574() )
        return(n224)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4563, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4552():
    try:
        n225 = __morloc_src_default_vector_test_vector_test.testEqual( "map U16"
        , m4293()
        , m4304()
        , m4563() )
        return(n225)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4552, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4541():
    try:
        n226 = __morloc_src_default_vector_test_vector_test.testEqual( "map U8"
        , m4271()
        , m4282()
        , m4552() )
        return(n226)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4541, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4530():
    try:
        n227 = __morloc_src_default_vector_test_vector_test.testEqual( "map I64"
        , m4249()
        , m4260()
        , m4541() )
        return(n227)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4530, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4519():
    try:
        n228 = __morloc_src_default_vector_test_vector_test.testEqual( "map I32"
        , m4227()
        , m4238()
        , m4530() )
        return(n228)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4519, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4508():
    try:
        n229 = __morloc_src_default_vector_test_vector_test.testEqual( "map I16"
        , m4205()
        , m4216()
        , m4519() )
        return(n229)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4508, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4497():
    try:
        n230 = __morloc_src_default_vector_test_vector_test.testEqual( "map I8"
        , m4183()
        , m4194()
        , m4508() )
        return(n230)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4497, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3848():
    try:
        n231 = __morloc_src_default_vector_test_vector_test.testEqual( "map Int"
        , m4161()
        , m4172()
        , m4497() )
        return(n231)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3848, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3861():
    try:
        n232 = __morloc_src_default_vector_test_vector_test.printMsg( "Foldable (fold sum)"
        , m3848() )
        return(n232)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3861, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4134():
    try:
        n233 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum empty"
        , m4059()
        , 0
        , m3861() )
        return(n233)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4134, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4128():
    try:
        n234 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum Real"
        , m4045()
        , 15.0
        , m4134() )
        return(n234)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4128, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4122():
    try:
        n235 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum F64"
        , m4029()
        , 15.0
        , m4128() )
        return(n235)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4122, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4116():
    try:
        n236 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum F32"
        , m4013()
        , 15.0
        , m4122() )
        return(n236)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4116, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4110():
    try:
        n237 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum U64"
        , m3997()
        , 15
        , m4116() )
        return(n237)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4110, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4104():
    try:
        n238 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum U32"
        , m3981()
        , 15
        , m4110() )
        return(n238)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4104, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4098():
    try:
        n239 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum U16"
        , m3965()
        , 15
        , m4104() )
        return(n239)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4098, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4092():
    try:
        n240 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum U8"
        , m3949()
        , 15
        , m4098() )
        return(n240)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4092, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4086():
    try:
        n241 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum I64"
        , m3933()
        , 15
        , m4092() )
        return(n241)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4086, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4080():
    try:
        n242 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum I32"
        , m3917()
        , 15
        , m4086() )
        return(n242)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4080, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4074():
    try:
        n243 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum I16"
        , m3901()
        , 15
        , m4080() )
        return(n243)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4074, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m4068():
    try:
        n244 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum I8"
        , m3885()
        , 15
        , m4074() )
        return(n244)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=4068, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3468():
    try:
        n245 = __morloc_src_default_vector_test_vector_test.testEqual( "fold sum Int"
        , m3869()
        , 15
        , m4068() )
        return(n245)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3468, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3481():
    try:
        n246 = __morloc_src_default_vector_test_vector_test.printMsg( "Eq"
        , m3468() )
        return(n246)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3481, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3832():
    try:
        n247 = __morloc_src_default_vector_test_vector_test.testEqual( "eq (Int,Str) diff"
        , False
        , m3690()
        , m3481() )
        return(n247)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3832, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3821():
    try:
        n248 = __morloc_src_default_vector_test_vector_test.testEqual( "eq (Int,Str) self"
        , True
        , m3668()
        , m3832() )
        return(n248)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3821, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3810():
    try:
        n249 = __morloc_src_default_vector_test_vector_test.testEqual( "eq Str diff"
        , False
        , m3652()
        , m3821() )
        return(n249)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3810, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3799():
    try:
        n250 = __morloc_src_default_vector_test_vector_test.testEqual( "eq Str self"
        , True
        , m3636()
        , m3810() )
        return(n250)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3799, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3788():
    try:
        n251 = __morloc_src_default_vector_test_vector_test.testEqual( "eq empty"
        , True
        , m3624()
        , m3799() )
        return(n251)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3788, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3777():
    try:
        n252 = __morloc_src_default_vector_test_vector_test.testEqual( "eq Bool diff"
        , False
        , m3608()
        , m3788() )
        return(n252)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3777, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3766():
    try:
        n253 = __morloc_src_default_vector_test_vector_test.testEqual( "eq Bool self"
        , True
        , m3592()
        , m3777() )
        return(n253)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3766, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3755():
    try:
        n254 = __morloc_src_default_vector_test_vector_test.testEqual( "eq Real diff"
        , False
        , m3576()
        , m3766() )
        return(n254)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3755, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3744():
    try:
        n255 = __morloc_src_default_vector_test_vector_test.testEqual( "eq Real self"
        , True
        , m3560()
        , m3755() )
        return(n255)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3744, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3733():
    try:
        n256 = __morloc_src_default_vector_test_vector_test.testEqual( "eq F64 self"
        , True
        , m3544()
        , m3744() )
        return(n256)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3733, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3722():
    try:
        n257 = __morloc_src_default_vector_test_vector_test.testEqual( "eq I64 self"
        , True
        , m3526()
        , m3733() )
        return(n257)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3722, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3711():
    try:
        n258 = __morloc_src_default_vector_test_vector_test.testEqual( "eq Int diff"
        , False
        , m3508()
        , m3722() )
        return(n258)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3711, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3462():
    try:
        n259 = __morloc_src_default_vector_test_vector_test.testEqual( "eq Int self"
        , True
        , m3490()
        , m3711() )
        return(n259)
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3462, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")

def m3449():
    try:
        n260 = __morloc_src_default_vector_test_vector_test.printResult(m3462())
        return(morloc.put_value(n260, mlc_schema_table[31]))
    except Exception as e:
            raise RuntimeError(f"{e!s}\n  at _ [py] (mid=3449, /opt/morloc-state/src/morloc/plane/default/internal/main.loc:83:16)")
# AUTO include manifolds end


# AUTO include dispatch start
dispatch = {
    3449: m3449,
}
remote_dispatch = {
}

# AUTO include dispatch end


def _with_debug_trace(msg: str) -> str:
    # Concatenate the morloc debug trace (if --debug was compiled in
    # and any frames were recorded) with the raised exception's
    # message. Returns msg unchanged when no trace is present. Each
    # manifold catch also appends its own "  at <name> [py] (mid=...,
    # srcloc)" line to the message via string concat, so non-debug
    # tracebacks still compose across pools.
    trace = morloc.debug_drain_frames()
    return f"{msg}\n{trace}" if trace else msg


def run_job(client_fd: int) -> None:
    try:
        # Free SHM from previous dispatch result (consumed by caller)
        morloc.shm_tracker_flush()
        morloc.debug_flush_dispatch()
        client_data = morloc.stream_from_client(client_fd)

        # If deferred source loading failed, every call fails with that error
        # (the program cannot run without its imports). Report it as a clean
        # fail packet rather than a cryptic dispatch error.
        if _mlc_source_error is not None and not morloc.is_ping(client_data):
            sys.stdout.flush()
            morloc.send_packet_to_foreign_server(client_fd, morloc.make_fail_packet(_mlc_source_error))
            return

        if(morloc.is_local_call(client_data)):
            (mid, args) = morloc.read_morloc_call_packet(client_data)

            try:
                result = dispatch[mid](*args)
            except Exception as e:
                result = morloc.make_fail_packet(_with_debug_trace(str(e)))

        elif(morloc.is_remote_call(client_data)):
            (mid, args) = morloc.read_morloc_call_packet(client_data)

            try:
                result = remote_dispatch[mid](*args)
            except Exception as e:
                result = morloc.make_fail_packet(_with_debug_trace(str(e)))

        elif(morloc.is_ping(client_data)):
            # The nexus abandons a readiness-ping connection when its probe
            # times out (common for slow-starting pools like Python) and
            # retries on a fresh connection. Answering the orphaned ping then
            # fails with a broken pipe -- benign, so send the pong silently and
            # do not route a failure through the loud "job failed" handler (the
            # C++/Rust pools already ignore pong-send failures the same way).
            sys.stdout.flush()
            try:
                morloc.send_packet_to_foreign_server(client_fd, morloc.pong(client_data))
            except Exception:
                pass
            return

        else:
            raise ValueError("Expected a ping or call type packet")

        # Flush stdout BEFORE sending the result back. The nexus prints its
        # own output (the return value) right after receiving this response.
        # Both processes share the same stdout fd, so if we flush after sending,
        # the nexus can print first, causing out-of-order output.
        sys.stdout.flush()

        morloc.send_packet_to_foreign_server(client_fd, result)

    except Exception as e:
        # Try to send a fail packet back to the caller before giving up.
        # This may fail (e.g., broken pipe from a timed-out ping), which is OK.
        try:
            result = morloc.make_fail_packet(str(e))
            morloc.send_packet_to_foreign_server(client_fd, result)
        except Exception:
            pass
        print(f"job failed: {e!s}", file=sys.stderr)
    finally:
        # Reclaim any stdio singleton claim this dispatch left open (e.g. a
        # handler that raised past @close on a broken pipe). Runs on the
        # same worker thread that opened it, so the reclaim's call_id gate
        # matches. Without this a leaked @stdout claim wedges every later
        # open with "@stdout already open in this nexus".
        morloc.reclaim_stdio_after_dispatch()
        # Safety-net flush for any output from error handling paths
        sys.stdout.flush()
        # close child copy
        morloc.close_socket(client_fd)


def _send_fd(sock, fd):
    """Send a file descriptor over a Unix domain socket."""
    sock.sendmsg([b'\x00'],
                 [(_socket.SOL_SOCKET, _socket.SCM_RIGHTS,
                   array.array('i', [fd]))])

def _recv_fd(sock):
    """Receive a file descriptor from a Unix domain socket."""
    msg, ancdata, flags, addr = sock.recvmsg(1, _socket.CMSG_SPACE(4))
    if not msg and not ancdata:
        raise EOFError("Connection closed")
    for cmsg_level, cmsg_type, cmsg_data in ancdata:
        if (cmsg_level == _socket.SOL_SOCKET and
                cmsg_type == _socket.SCM_RIGHTS):
            a = array.array('i')
            a.frombytes(cmsg_data[:4])
            return a[0]
    raise RuntimeError("No fd received in ancillary data")


WORKER_IDLE_TIMEOUT = 5.0  # seconds before an idle worker exits

def worker_process(job_fd, tmpdir, shm_basename, shutdown_flag, busy_count, total_workers, wakeup_w):
    # Reset signal handlers inherited from main. If user code inside run_job
    # calls multiprocessing.Pool (or anything else that forks and later
    # SIGTERMs its own children), those grandchildren would otherwise inherit
    # main's signal_handler and flip the shared shutdown_flag, causing main
    # to SIGKILL this worker mid-response. See the multiprocessing-py-1 bug.
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    morloc.set_fallback_dir(tmpdir)
    morloc.shinit(shm_basename, 0, 0xffff)
    # Load user sources HERE, post-fork, in the worker's own process (see the
    # _mlc_user_sources note). A failure is recorded, not raised: run_job turns
    # it into a fail packet so the caller gets the real import error.
    _mlc_load_user_sources()
    _init_worker_tracking(busy_count, total_workers, wakeup_w)
    sock = _socket.fromfd(job_fd, _socket.AF_UNIX, _socket.SOCK_STREAM)
    os.close(job_fd)  # sock owns a dup'd copy
    last_activity = time.monotonic()
    try:
        # poll() (not select.select) avoids the FD_SETSIZE=1024 ceiling: a
        # job-queue fd >= 1024 makes select.select raise ValueError and kill the
        # worker. The Rust/C/R pools already use poll(2) for this reason.
        poller = select.poll()
        poller.register(sock.fileno(), select.POLLIN)
        while not shutdown_flag.value:
            events = poller.poll(10)  # milliseconds (was 0.01s)
            if shutdown_flag.value:
                break
            if events:
                try:
                    client_fd = _recv_fd(sock)
                    run_job(client_fd)
                    last_activity = time.monotonic()
                except (EOFError, OSError):
                    break
            elif total_workers.value > 1 and time.monotonic() - last_activity > WORKER_IDLE_TIMEOUT:
                break
    except BaseException as e:
        # Catch-all for errors that escape run_job's own exception handling:
        # MemoryError, KeyboardInterrupt, SystemExit, or bugs in the worker
        # loop itself. Without this, the worker dies silently and the nexus
        # only sees "failed to read response header" with no indication of
        # what went wrong in the pool.
        #
        # Race condition: the nexus detects the broken socket and may start
        # its clean_exit tear-down (SIGTERM -> SIGKILL) while this print is
        # still buffered. We flush immediately to maximize the chance the
        # message reaches the terminal before we are killed. stderr is
        # line-buffered (set in __main__), but the flush is a safety net for
        # edge cases (redirected stderr, forked-process buffer state).
        import traceback
        print(f"morloc pool worker fatal error: {e!s}", file=sys.stderr)
        traceback.print_exc(file=sys.stderr)
        sys.stderr.flush()
    finally:
        # This worker is a process of its own and is leaving; its deferred
        # releases have no next dispatch to perform them, and a process
        # exiting does not decrement a reference other processes can see.
        # The worker is idle or already failing here, so nothing it holds is
        # still in use by a dispatch of its own.
        try:
            morloc.shm_tracker_flush()
        except Exception:
            pass
        sock.close()


def _hold_daemon(d):
    _daemon_slot.append(d)
    return d


def _take_daemon():
    # None when someone else already took it.
    try:
        return _daemon_slot.pop()
    except IndexError:
        return None


def signal_handler(sig, frame):
    global daemon
    # Re-entrancy is safe here and needs no change to signal dispositions.
    # Python processes pending signals between bytecodes, including while a
    # handler is running, so a second SIGTERM can re-enter this function at any
    # point; every step below is idempotent, and the daemon pointer is taken
    # from a slot that yields it to exactly one caller. Setting SIGTERM to
    # SIG_IGN here instead would race with the delivery of that second signal,
    # which the interpreter reports by printing
    # "OSError: Signal 15 ignored due to race condition" to stderr.
    shutdown_flag.value = True
    if _shutdown_wakeup_fd >= 0:
        try:
            os.write(_shutdown_wakeup_fd, b'!')
        except OSError:
            pass
    daemon = None
    d = _take_daemon()
    if d is not None:
        morloc.close_daemon(d)


def client_listener(job_fd, socket_path, tmpdir, shm_basename, shutdown_flag):
    global daemon
    daemon = _hold_daemon(morloc.start_daemon(socket_path, tmpdir, shm_basename, 0xffff))
    sock = _socket.fromfd(job_fd, _socket.AF_UNIX, _socket.SOCK_STREAM)
    os.close(job_fd)  # sock owns a dup'd copy

    while not shutdown_flag.value:
        try:
            client_fd = morloc.wait_for_client(daemon)
        except Exception as e:
            print(f"In python daemon, failed to connect to client: {e!s}", file=sys.stderr)
            continue

        if client_fd > 0:
            try:
                _send_fd(sock, client_fd)
            except Exception as e:
                print(f"In python daemon, failed to start worker: {e!s}", file=sys.stderr)
            finally:
                morloc.close_socket(client_fd)
    sock.close()



def _select_pool_mode():
    # Concurrency model for the Python pool:
    #   MORLOC_PY_POOL=fork    -> process workers (the fork model; works on Linux
    #                             and lets user code fork freely)
    #   MORLOC_PY_POOL=thread  -> thread workers (no live-interpreter fork; safe
    #                             on macOS, where forking a live CPython aborts)
    # Default: thread on macOS, fork elsewhere. The env var overrides the default
    # so BOTH models are exercisable on BOTH platforms (test the thread model on
    # Linux CI; force fork on macOS to reconfirm the crash).
    mode = os.environ.get("MORLOC_PY_POOL", "").strip().lower()
    if mode in ("fork", "thread"):
        return mode
    return "thread" if sys.platform == "darwin" else "fork"


def run_thread_pool(socket_path, tmpdir, shm_basename):
    # Thread-based pool: workers are threads, so a live CPython interpreter is
    # never forked (that abort is the macOS pool-death bug). Re-entrant callbacks
    # are served because foreign_call releases the GIL (pymorloc), letting a
    # sibling worker run while one is blocked on a downstream pool. Per-dispatch
    # state (shm_tracker, recur_env) is __thread in pymorloc, so workers do not
    # corrupt one another; libmorloc's own dispatch state is already thread-safe
    # (the C++ pool runs this same threaded model). User multi-threading/parallel
    # map still works: multiprocessing uses the 'fork' start method (forced in
    # __main__) so a user Pool worker inherits the manifolds exec'd into this
    # process's globals; spawn would re-import and fail to find them. Forking
    # this threaded pool is safe on macOS (OBJC_DISABLE_INITIALIZE_FORK_SAFETY;
    # the pool's threads hold no Apple-framework locks at fork time).
    #
    # Concurrency note: worker manifolds run on multiple threads sharing one
    # interpreter, so user Python source is expected to be thread-safe under
    # concurrent/re-entrant dispatch (morloc manifolds are pure functions, and
    # numpy/most C extensions release the GIL and are safe). Source that mutates
    # module-level globals from a manifold is the exception. This is inherent to
    # the thread model -- the process-isolated fork model is not available on
    # macOS (forking a live interpreter aborts), and serializing dispatch would
    # forfeit the required in-pool parallelism.
    morloc.set_fallback_dir(tmpdir)
    morloc.shinit(shm_basename, 0, 0xffff)  # attach SHM once for the process
    _mlc_load_user_sources()  # no fork on this path -> safe to import in-thread

    daemon = _hold_daemon(morloc.start_daemon(socket_path, tmpdir, shm_basename, 0xffff))

    stop = threading.Event()
    def _on_signal(_sig, _frame):
        stop.set()
    signal.signal(signal.SIGTERM, _on_signal)
    signal.signal(signal.SIGINT, _on_signal)

    job_q = queue.Queue()
    sched = threading.Lock()
    counts = {"busy": 0, "total": 0}

    def _spawn_worker():
        threading.Thread(target=_worker_loop, daemon=True).start()

    def _worker_loop():
        with sched:
            counts["total"] += 1
        released = False  # ensure this worker's slot is freed exactly once
        try:
            while not stop.is_set():
                try:
                    client_fd = job_q.get(timeout=0.1)
                except queue.Empty:
                    # Reap a surplus idle worker (always keep at least one) so a
                    # re-entrancy burst does not leave threads around forever.
                    with sched:
                        if counts["total"] > 1:
                            counts["total"] -= 1
                            released = True
                            # shm_tracker is __thread and its SHM is freed lazily
                            # on the NEXT dispatch; a reaped worker has none, so
                            # flush its last job's SHM now rather than leak it
                            # until pool shutdown.
                            morloc.shm_tracker_flush()
                            return
                    continue
                # Reserve a slot; if that saturates the pool, pre-spawn one so a
                # re-entrant callback always finds a free worker (no deadlock).
                with sched:
                    counts["busy"] += 1
                    saturated = counts["busy"] >= counts["total"]
                if saturated:
                    _spawn_worker()
                try:
                    run_job(client_fd)
                finally:
                    with sched:
                        counts["busy"] -= 1
        except BaseException as e:
            import traceback
            print(f"morloc pool worker thread fatal: {e!s}", file=sys.stderr)
            traceback.print_exc(file=sys.stderr)
            sys.stderr.flush()
        finally:
            # A thread leaving by any route takes its deferred releases with
            # it, so perform them here as the surplus reap above does.
            try:
                morloc.shm_tracker_flush()
            except Exception:
                pass
            # Release the slot on a FATAL exit too (the surplus reap above already
            # released it and set the flag). Leaking `total` would make the
            # saturation gate `busy >= total` stop tripping, so a re-entrant
            # callback could find no free worker and deadlock.
            if not released:
                with sched:
                    if counts["total"] > 0:
                        counts["total"] -= 1

    def _listener_loop():
        # Single acceptor: pull each client off the daemon and hand it to a
        # worker via the in-process queue (no fd-passing needed among threads).
        # A 200ms accept timeout lets this thread observe `stop` and exit so it
        # can be joined BEFORE close_daemon frees the daemon struct (otherwise it
        # would dereference freed memory on shutdown).
        while not stop.is_set():
            try:
                client_fd = morloc.wait_for_client(daemon, 200000)
            except Exception as e:
                print(f"In python daemon, failed to connect to client: {e!s}", file=sys.stderr)
                continue
            if client_fd and client_fd > 0:
                job_q.put(client_fd)

    _spawn_worker()
    listener = threading.Thread(target=_listener_loop, daemon=True)
    listener.start()

    try:
        while not stop.is_set():
            time.sleep(0.05)
    finally:
        stop.set()
        # Join the listener so it is no longer inside wait_for_client(daemon)
        # before we free the daemon struct. It polls `stop` every 200ms, so this
        # returns promptly; the bound keeps a wedged listener from hanging exit.
        listener.join(timeout=2.0)
        sys.stdout.flush()
        d = _take_daemon()
        if d is not None:
            try:
                morloc.close_daemon(d)
            except Exception:
                pass


if __name__ == "__main__":
    # Line-buffer stderr so diagnostic output is not lost when pool is killed.
    # stdout is left fully buffered for performance (genome-scale piping) and
    # flushed explicitly after each job and during shutdown.
    sys.stderr.reconfigure(line_buffering=True)

    # Force the 'fork' multiprocessing start method on ALL platforms. Two
    # reasons, both requiring fork:
    #   1. The fork pool hands each worker the shared job-queue socketpair BY FD
    #      NUMBER and relies on inheritance; spawn (macOS default) gives a fresh
    #      interpreter with no inherited fds (socket.fromfd -> EBADF/ENOTSOCK).
    #   2. User multiprocessing.Pool must be able to run morloc manifolds, which
    #      are exec'd into this process's globals (not an importable module).
    #      Under spawn the mp worker re-imports and cannot find them
    #      ("name 'foo' is not defined"); under fork it inherits them. This is
    #      the user-parallel-map capability, which must work on macOS too.
    # Forking this (thread-model) pool process is safe on macOS in practice: the
    # nexus exports OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES and the pool's own
    # threads are plain socket-I/O pthreads holding no Apple-framework locks at
    # fork time. On Linux fork is already the default (no-op).
    import multiprocessing as _mp
    try:
        _mp.set_start_method("fork", force=True)
    except (RuntimeError, ValueError):
        pass

    # Request SIGTERM when the parent process (nexus) dies (Linux only).
    # Without this, SIGKILL on the nexus leaves pool processes orphaned and
    # their SHM segments leak. macOS has no prctl/PR_SET_PDEATHSIG equivalent,
    # so this is a known parity gap there: a SIGKILL'd nexus can orphan pools
    # (normal shutdown is still clean via the nexus's process-group teardown).
    try:
        import ctypes
        _PR_SET_PDEATHSIG = 1
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(_PR_SET_PDEATHSIG, signal.SIGTERM)
    except Exception:
        pass  # non-Linux (e.g. macOS): no PDEATHSIG -- see the note above

    # RawValue (no lock), not Value: the SIGTERM/SIGINT handler writes this flag,
    # and a Value's semaphore lock is not async-signal-safe -- acquiring it in the
    # handler while the interrupted code already holds it can deadlock/corrupt at
    # teardown. A single-byte flag needs no lock (byte writes are atomic).
    shutdown_flag = RawValue('b', False)  # Shared flag (lock-free)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Health check: confirm imports loaded and print version
    if len(sys.argv) > 1 and sys.argv[1] == "--health":
        # Actually load the user sources so --health still validates that the
        # program's imports resolve (they no longer run at module import).
        _mlc_load_user_sources()
        if _mlc_source_error is not None:
            sys.stdout.write('{"status":"error","version":"0.105.2"}\n')
            print(_mlc_source_error, file=sys.stderr)
            sys.exit(1)
        sys.stdout.write('{"status":"ok","version":"0.105.2"}\n')
        sys.exit(0)

    # Process arguments passed from the nexus
    try:
        socket_path = sys.argv[1]
        tmpdir = sys.argv[2]
        shm_basename = sys.argv[3]
    except IndexError:
        print("Usage: script.py <socket_path> <tmpdir> <shm_basename>")
        sys.exit(1)

    global_state["tmpdir"] = tmpdir

    # Thread-based pool (macOS default; forced anywhere via MORLOC_PY_POOL=thread).
    # Avoids forking a live CPython interpreter. Everything below this branch is
    # the fork-based process pool (Linux default / MORLOC_PY_POOL=fork).
    if _select_pool_mode() == "thread":
        run_thread_pool(socket_path, tmpdir, shm_basename)
        sys.exit(0)

    # Shared job queue: listener writes fds to write_sock, workers read from read_sock.
    # Only idle workers (blocked in recvmsg) pick up jobs, preventing the round-robin
    # deadlock where a callback gets dispatched to a busy worker.
    read_sock, write_sock = _socket.socketpair(_socket.AF_UNIX, _socket.SOCK_STREAM)

    num_workers = 1
    workers = []

    # Shared counters for dynamic worker spawning.
    # Workers increment busy_count before foreign_call and decrement after.
    # When all workers are busy, main process spawns a new one.
    busy_count = RawValue(ctypes.c_int, 0)
    total_workers = RawValue(ctypes.c_int, num_workers)
    wakeup_r, wakeup_w = os.pipe()
    os.set_blocking(wakeup_r, False)
    _shutdown_wakeup_fd = wakeup_w

    # Keep a dup of the read end so we can spawn new workers later
    spare_read_fd = os.dup(read_sock.fileno())

    for i in range(num_workers):
        worker = Process(target=worker_process,
                         args=(read_sock.fileno(), tmpdir, shm_basename, shutdown_flag,
                               busy_count, total_workers, wakeup_w))
        worker.start()
        workers.append(worker)
    read_sock.close()  # main/listener don't need the read end (spare_read_fd kept)

    # Start client listener process
    listener_process = Process(
        target=client_listener,
        args=(write_sock.fileno(), socket_path, tmpdir, shm_basename, shutdown_flag)
    )
    listener_process.start()
    write_sock.close()  # main doesn't need the write end

    # Main loop: monitor wake-up pipe, spawn new workers when all are busy,
    # and reap idle workers that have exited. poll() (not select.select) avoids
    # the FD_SETSIZE=1024 ceiling.
    wakeup_poller = select.poll()
    wakeup_poller.register(wakeup_r, select.POLLIN)
    while not shutdown_flag.value:
        events = wakeup_poller.poll(10)  # milliseconds (was 0.01s)
        if events:
            try:
                os.read(wakeup_r, 4096)  # drain pipe
            except OSError:
                pass

        # Reap dead workers (idle timeout or error exit)
        alive = []
        for w in workers:
            if w.is_alive():
                alive.append(w)
            else:
                w.join(timeout=0)
                w.close()
        workers = alive
        total_workers.value = max(1, len(workers))

        # Spawn a new worker if all are busy (or all have exited)
        if len(workers) == 0 or busy_count.value >= total_workers.value:
            w = Process(target=worker_process,
                        args=(spare_read_fd, tmpdir, shm_basename, shutdown_flag,
                              busy_count, total_workers, wakeup_w))
            w.start()
            workers.append(w)
            total_workers.value = len(workers)

    # Shutdown sequence
    os.close(wakeup_r)
    os.close(wakeup_w)
    os.close(spare_read_fd)

    # 1. Stop listener first
    listener_process.terminate()
    listener_process.join(timeout=0.001)
    listener_process.kill()
    listener_process.join()  # Final blocking reap
    listener_process.close()

    # 2. Terminate workers with escalating force
    for p in workers:
        if p.is_alive():
            p.kill()
        p.join()  # Final blocking reap
        p.close()

    sys.exit(0)
