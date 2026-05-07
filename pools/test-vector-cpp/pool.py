import signal
import sys
import select
import os # required for setting path to morloc dependencies
import time
import copy
import array
import struct
import socket as _socket
from collections import OrderedDict
from multiprocessing import Process, Value, RawValue
import ctypes
import functools


# Global variables for clean signal handling
daemon = None
workers = []
global_state = dict()
_shutdown_wakeup_fd = -1

# AUTO include sources start
sys.path = [os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")), os.path.expanduser("."), os.path.expanduser("/home/dev/.local/share/morloc/opt"), os.path.expanduser("/home/dev/.local/share/morloc/src/morloc/plane")] + sys.path
import importlib
import pymorloc as morloc
default_vector_test_vector_test = importlib.import_module("default.vector.test.vector-test")
mlc_schema_table = [ "<numpy.ndarray>a:0<float>f8"
, "<numpy.ndarray>a:3<float>f8"
, "<numpy.ndarray>a:4<float>f8"
, "<numpy.ndarray>a:8<float>f8"
, "<numpy.ndarray>a:1<float>f8"
, "<numpy.ndarray>a:6<float>f8"
, "<tuple>t2<int>j<int>j" ]
# AUTO include sources end

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

# AUTO include manifolds start
def m1720():
    try:
        s12 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1720
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1720):\n{e!s}")
    return(morloc.get_value(s12, mlc_schema_table[0]))

def m1730():
    try:
        s14 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1730
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1730):\n{e!s}")
    return(morloc.get_value(s14, mlc_schema_table[1]))

def m1743():
    try:
        s16 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1743
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1743):\n{e!s}")
    return(morloc.get_value(s16, mlc_schema_table[2]))

def m1755():
    try:
        s18 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1755
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1755):\n{e!s}")
    return(morloc.get_value(s18, mlc_schema_table[3]))

def m1810():
    try:
        s20 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1810
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1810):\n{e!s}")
    return(morloc.get_value(s20, mlc_schema_table[0]))

def m1819():
    try:
        s22 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1819
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1819):\n{e!s}")
    return(morloc.get_value(s22, mlc_schema_table[4]))

def m1829():
    try:
        s24 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1829
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1829):\n{e!s}")
    return(morloc.get_value(s24, mlc_schema_table[1]))

def m1839():
    try:
        s26 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1839
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1839):\n{e!s}")
    return(morloc.get_value(s26, mlc_schema_table[5]))

def m1885():
    try:
        s28 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1885
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1885):\n{e!s}")
    return(morloc.get_value(s28, mlc_schema_table[0]))

def m1894():
    try:
        s30 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1894
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1894):\n{e!s}")
    return(morloc.get_value(s30, mlc_schema_table[4]))

def m1904():
    try:
        s32 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1904
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1904):\n{e!s}")
    return(morloc.get_value(s32, mlc_schema_table[1]))

def m1914():
    try:
        s34 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1914
        , [] )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1914):\n{e!s}")
    return(morloc.get_value(s34, mlc_schema_table[5]))

def m1877():
    try:
        s36 = morloc.foreign_call( os.path.join(global_state["tmpdir"], "pipe-cpp")
        , 1533
        , [] )
        n37 = default_vector_test_vector_test.printMsg( "Zeros"
        , morloc.get_value(s36, mlc_schema_table[6]) )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1877):\n{e!s}")
    return(n37)

def m1939():
    try:
        n35 = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
        n38 = default_vector_test_vector_test.testEqual( "zeros1 6"
        , m1914()
        , n35
        , m1877() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1939):\n{e!s}")
    return(n38)

def m1933():
    try:
        n33 = [0.0, 0.0, 0.0]
        n39 = default_vector_test_vector_test.testEqual( "zeros1 3"
        , m1904()
        , n33
        , m1939() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1933):\n{e!s}")
    return(n39)

def m1927():
    try:
        n31 = [0.0]
        n40 = default_vector_test_vector_test.testEqual( "zeros1 1"
        , m1894()
        , n31
        , m1933() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1927):\n{e!s}")
    return(n40)

def m1789():
    try:
        n29 = []
        n41 = default_vector_test_vector_test.testEqual( "zeros1 0"
        , m1885()
        , n29
        , m1927() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1789):\n{e!s}")
    return(n41)

def m1802():
    try:
        n42 = default_vector_test_vector_test.printMsg("Ones", m1789())
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1802):\n{e!s}")
    return(n42)

def m1864():
    try:
        n27 = [1.0, 1.0, 1.0, 1.0, 1.0, 1.0]
        n43 = default_vector_test_vector_test.testEqual( "ones1 6"
        , m1839()
        , n27
        , m1802() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1864):\n{e!s}")
    return(n43)

def m1858():
    try:
        n25 = [1.0, 1.0, 1.0]
        n44 = default_vector_test_vector_test.testEqual( "ones1 3"
        , m1829()
        , n25
        , m1864() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1858):\n{e!s}")
    return(n44)

def m1852():
    try:
        n23 = [1.0]
        n45 = default_vector_test_vector_test.testEqual( "ones1 1"
        , m1819()
        , n23
        , m1858() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1852):\n{e!s}")
    return(n45)

def m1699():
    try:
        n21 = []
        n46 = default_vector_test_vector_test.testEqual( "ones1 0"
        , m1810()
        , n21
        , m1852() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1699):\n{e!s}")
    return(n46)

def m1712():
    try:
        n47 = default_vector_test_vector_test.printMsg("Fill", m1699())
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1712):\n{e!s}")
    return(n47)

def m1783():
    try:
        n19 = [0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5]
        n48 = default_vector_test_vector_test.testEqual( "fill1 0.5 8"
        , m1755()
        , n19
        , m1712() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1783):\n{e!s}")
    return(n48)

def m1777():
    try:
        n17 = [-1.5, -1.5, -1.5, -1.5]
        n49 = default_vector_test_vector_test.testEqual( "fill1 -1.5 4"
        , m1743()
        , n17
        , m1783() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1777):\n{e!s}")
    return(n49)

def m1771():
    try:
        n15 = [7.0, 7.0, 7.0]
        n50 = default_vector_test_vector_test.testEqual( "fill1 7.0 3"
        , m1730()
        , n15
        , m1777() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1771):\n{e!s}")
    return(n50)

def m1586():
    try:
        n13 = []
        n51 = default_vector_test_vector_test.testEqual( "fill1 0.0 0"
        , m1720()
        , n13
        , m1771() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1586):\n{e!s}")
    return(n51)

def m1599():
    try:
        n52 = default_vector_test_vector_test.printMsg( "Round-trip (non-uniform)"
        , m1586() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1599):\n{e!s}")
    return(n52)

def m1693(n11, n8):
    try:
        n53 = default_vector_test_vector_test.testEqual( "Vector 16 Real round-trip"
        , n8
        , n11
        , m1599() )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1693):\n{e!s}")
    return(n53)

def m1687(n11, n10, n8, n7):
    try:
        n54 = default_vector_test_vector_test.testEqual( "Vector 8 Real round-trip"
        , n7
        , n10
        , m1693(n11, n8) )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1687):\n{e!s}")
    return(n54)

def m1580(n11, n10, n9, n8, n7, n6):
    try:
        n55 = default_vector_test_vector_test.testEqual( "Vector 4 Real round-trip"
        , n6
        , n9
        , m1687(n11, n10, n8, n7) )
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1580):\n{e!s}")
    return(n55)

def m1567():
    try:
        n6 = [1.0, 2.0, 3.0, 4.0]
        n7 = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]
        n8 = [ 1.0
        , 2.0
        , 3.0
        , 4.0
        , 5.0
        , 6.0
        , 7.0
        , 8.0
        , 9.0
        , 10.0
        , 11.0
        , 12.0
        , 13.0
        , 14.0
        , 15.0
        , 16.0 ]
        n9 = [1.0, 2.0, 3.0, 4.0]
        n10 = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]
        n11 = [ 1.0
        , 2.0
        , 3.0
        , 4.0
        , 5.0
        , 6.0
        , 7.0
        , 8.0
        , 9.0
        , 10.0
        , 11.0
        , 12.0
        , 13.0
        , 14.0
        , 15.0
        , 16.0 ]
        n56 = m1580(n11, n10, n9, n8, n7, n6)
        n57 = default_vector_test_vector_test.printResult(n56)
    except Exception as e:
            raise RuntimeError(f"Error (pool daemon in m1567):\n{e!s}")
    return(morloc.put_value(n57, mlc_schema_table[6]))
# AUTO include manifolds end


# AUTO include dispatch start
dispatch = {
    1567: m1567,
}
remote_dispatch = {
}
# AUTO include dispatch end

def run_job(client_fd: int) -> None:
    try:
        # Free SHM from previous dispatch result (consumed by caller)
        morloc.flush_shm_tracker()
        client_data = morloc.stream_from_client(client_fd)

        if(morloc.is_local_call(client_data)):
            (mid, args) = morloc.read_morloc_call_packet(client_data)

            try:
                result = dispatch[mid](*args)
            except Exception as e:
                result = morloc.make_fail_packet(str(e))

        elif(morloc.is_remote_call(client_data)):
            (mid, args) = morloc.read_morloc_call_packet(client_data)

            try:
                result = remote_dispatch[mid](*args)
            except Exception as e:
                result = morloc.make_fail_packet(str(e))

        elif(morloc.is_ping(client_data)):
            result = morloc.pong(client_data)

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
    _init_worker_tracking(busy_count, total_workers, wakeup_w)
    sock = _socket.fromfd(job_fd, _socket.AF_UNIX, _socket.SOCK_STREAM)
    os.close(job_fd)  # sock owns a dup'd copy
    last_activity = time.monotonic()
    try:
        while not shutdown_flag.value:
            rlist, _, _ = select.select([sock.fileno()], [], [], 0.01)
            if shutdown_flag.value:
                break
            if rlist:
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
        sock.close()


def signal_handler(sig, frame):
    global daemon
    # Ignore further SIGTERM/SIGINT during cleanup. Python processes pending
    # signals between bytecodes, including while another signal handler is
    # running, so a second SIGTERM arriving mid-cleanup would otherwise
    # re-enter this handler and double-free the daemon pointer.
    try:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
    except Exception:
        pass
    shutdown_flag.value = True
    if _shutdown_wakeup_fd >= 0:
        try:
            os.write(_shutdown_wakeup_fd, b'!')
        except OSError:
            pass
    # Capture the daemon pointer into a local and clear the global BEFORE
    # invoking close_daemon. If a pending signal still slips through and
    # re-enters this handler, it will see daemon=None and skip the free.
    d = daemon
    daemon = None
    if d is not None:
        morloc.close_daemon(d)


def client_listener(job_fd, socket_path, tmpdir, shm_basename, shutdown_flag):
    global daemon
    daemon = morloc.start_daemon(socket_path, tmpdir, shm_basename, 0xffff)
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



if __name__ == "__main__":
    # Line-buffer stderr so diagnostic output is not lost when pool is killed.
    # stdout is left fully buffered for performance (genome-scale piping) and
    # flushed explicitly after each job and during shutdown.
    sys.stderr.reconfigure(line_buffering=True)

    # Request SIGTERM when the parent process (nexus) dies.
    # Without this, SIGKILL on the nexus leaves pool processes orphaned
    # and their SHM segments leak in /dev/shm.
    try:
        import ctypes
        _PR_SET_PDEATHSIG = 1
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(_PR_SET_PDEATHSIG, signal.SIGTERM)
    except Exception:
        pass  # non-Linux: skip (macOS uses kqueue for this)

    shutdown_flag = Value('b', False)  # Shared flag

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Health check: confirm imports loaded and print version
    if len(sys.argv) > 1 and sys.argv[1] == "--health":
        sys.stdout.write('{"status":"ok","version":"0.82.0"}\n')
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
    # and reap idle workers that have exited.
    while not shutdown_flag.value:
        rlist, _, _ = select.select([wakeup_r], [], [], 0.01)
        if rlist:
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
