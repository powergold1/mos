#!/usr/bin/python

import asyncio
import dis
import hashlib
import os
import subprocess
import traceback
import sys
from typing import List, Dict, Set, Tuple, Coroutine, Awaitable, Any, Callable, Optional

import msgspec.json

running: Dict[str, Awaitable[Tuple[str, int]]] = {}
known_mtimes: Dict[str, float] = {}
dummytime = float('nan')
# for each target, a map from dependency to mtime
alldeps: Dict[str, Dict[str, float]] = {}
seen: Set[str] = set()

sema = asyncio.Semaphore(os.cpu_count() or 1)
#sema = asyncio.Semaphore(1)

CC = "/usr/bin/clang"
if "clang" in CC:
    DIAG=["-fno-caret-diagnostics"]
    LINK=["--ld-path=/usr/bin/mold"]
elif "gcc" in CC:
    DIAG=["-fno-diagnostics-show-caret"]
    LINK=["-fuse-ld=mold"]
WARN=[
    "-Wall", "-Wextra", "-Werror",
    "-Wno-cast-align", "-Wno-cast-qual", "-Wno-unused-parameter",
    "-Wno-unused-function", "-Wno-unused-variable", "-Wshadow",
    "-Wpointer-arith", "-Wstrict-prototypes", "-Wmissing-prototypes",
]
DEFS=["-fno-exceptions", "-mfma", "-std=c2x"]
# TODO dbg and rel
OPT_DBG = ["-g"]

async def aspawn(prog: str, cmd: List[str]) -> Tuple[int, Optional[bytes], Optional[bytes]]:
    async with sema:
        try:
            # print("calling", prog, *cmd)
            proc = await asyncio.create_subprocess_exec(prog, *cmd, stderr=subprocess.PIPE, stdout=None)
            proc_stdout, proc_stderr = await proc.communicate()
            rc = proc.returncode if proc.returncode is not None else -1
            return rc, proc_stdout, proc_stderr
        except Exception as e:
            # internal error
            traceback.print_exception(e)
            return 1, None, None


def clear_mtime(target: str):
    global known_mtimes
    try:
        del known_mtimes[target]
    except KeyError:
        pass


def read_deps_file(dep: str) -> Optional[List[str]]:
    try:
        with open(dep, "r") as f:
            raw = f.read()
        deps_str = raw[raw.index(":")+1:].replace("\\\n"," ")
        deps_list = list(filter(lambda x: x, map(lambda x: x.strip(), deps_str.split(" "))))
        return deps_list
    except FileNotFoundError:
        return None


async def default_o(target: str) -> Tuple[str, int]:
    assert target.startswith("bld/")
    assert target.endswith(".o")
    assert target.endswith("dbg.o") or target.endswith("rel.o")
    dep = target[:-2] + ".d"
    deps = read_deps_file(dep)
    if deps is not None:
        err = await redo_ifchange(target, deps)
        if err != 0:
            return target, 1
    else:
        # ok, .d file does not exist yet
        pass
    src = "src/" + target[4:-6] + ".c"
    depflag=["-MMD",  "-MF", dep, "-MT", target]
    # TODO: dbg and rel
    compiler_args = [*depflag, *DEFS, *DIAG, *WARN, *OPT_DBG, "-c", src, "-o", target]
    rc, proc_stdout, proc_stderr = await aspawn(CC, compiler_args)
    if rc:
        if proc_stderr is not None:
            sys.stderr.buffer.write(proc_stderr)
        return target, rc
    clear_mtime(target)
    clear_mtime(dep)
    await redo_ifchange(target, [dep])
    # this should track the dependencies when the .d file is created initially.
    err = 0
    if deps is None:
        deps = read_deps_file(dep)
        assert deps is not None
        err = await redo_ifchange(target, deps)
        return target, err
    return target, err



async def do_exe(target: str, objs: List[str], opt: List[str], libs: List[str]) -> Tuple[str, int]:
    err = await redo_ifchange(target, objs)
    if err != 0:
        return target, err
    compiler_args = [ *DIAG, *LINK, *WARN, *opt, *objs, "-o", target, *libs ]
    rc, proc_stdout, proc_stderr = await aspawn(CC, compiler_args)
    if rc and proc_stderr is not None:
        sys.stderr.buffer.write(proc_stderr)
    clear_mtime(target)
    return target, rc


async def do_mos(target: str) -> Tuple[str, int]:
    assert target == "mos"
    # TODO: this is a little incorrect. it doesn't trigger a rebuild if i just
    # add a new file here, because in this implementation, the target doesn't
    # depend on the build script itself.
    #
    # I could perhaps add an implicit dependency on the do_function that
    # creates a target.  that dependency would have to be tracked with a hash,
    # since i have everything in a single file, so i don't have mtimes for
    # functions.  that's one downside of having everything in a single file.
    # Or alternatively, i make a normal mtime dependency on this entire build
    #
    # script itself. upside would be that it's easy to implement. downside
    # would be that changing a single character in this script would mean
    # everything is now out of date.

    objs = [ "def", "mos" ]
    dbg_objs = ["bld/" + x + ".dbg.o" for x in objs]
    # TODO: dbg and rel
    return await do_exe(target, dbg_objs, OPT_DBG, ["-L/usr/local/lib", "-lSDL3", "-lSDL3_ttf", "-lavcodec", "-lavformat", "-lavutil"])


async def default(target: str) -> Tuple[str, int]:
    global alldeps
    if os.path.exists(target):
        alldeps[target] = {}
        return target, 0
    else:
        raise ValueError(target + " does not exist and there is no rule to make it")


RULES: Dict[str, Callable[[str], Coroutine[Any, Any, Tuple[str, int]]]] = {
    "default.o": default_o,
    "mos": do_mos,
}
ALL_TARGETS: List[str] = ["mos"]


def get_rule(target: str) -> Callable[[str], Coroutine[Any, Any, Tuple[str,int]]]:
    exact = RULES.get(target)
    if (exact := RULES.get(target)) is not None:
        return exact
    _, ext = os.path.splitext(target)
    if (rule := RULES.get("default" + ext)) is not None:
        return rule
    return default


def getmtime(x: str) -> float:
    if (known := known_mtimes.get(x)) != None:
        return known
    try:
        t = os.path.getmtime(x)
    except FileNotFoundError:
        t = dummytime
    known_mtimes[x] = t
    return t


def is_up_to_date(target: str) -> bool:
    global alldeps
    if not os.path.exists(target):
        return False
    deps = alldeps.get(target)
    if deps is None:
        return False
    assert isinstance(deps, dict)
    ok = True
    for depkey, mtime in deps.items():
        # Did the dependency itself get touched or removed?
        try:
            if getmtime(depkey) != mtime:
                ok = False
                break
        except FileNotFoundError:
            ok = False
            break
        # Recurse into dependencies of dependency
        if not is_up_to_date(depkey):
            ok = False
            break
    return ok


async def redo_ifchange(me: str, targets: List[str]) -> int:
    global alldeps
    global seen
    assert isinstance(me, str)
    assert isinstance(targets, list)
    futs = []
    mydeps = {}
    for target in targets:
        mydeps[target] = getmtime(target)
        if is_up_to_date(target):
            continue
        if (fut := running.get(target)) is None:
            maker = get_rule(target)
            # this confused me, but we can just store the task here.
            # Task in python is future-like.  It runs a coroutine, but doesn't
            # immediately go away afterwards.  the result of the Task object
            # can be referenced and awaited later multiple times.
            print(target)
            fut = asyncio.create_task(maker(target))
        futs.append(fut)
    err = 0
    async for fut in asyncio.as_completed(futs):
        # TODO: if we want to rebuild something more than once for some reason,
        # because for some reason the build rule has to be invoked iterateively,
        # we'd have to remove fut from running here, otherwise the cached result
        # will always be the same.
        exc = fut.exception()
        if exc is not None:
            print("exception ", exc, file=sys.stderr)
            err = 1
            continue
        target, depsuccess = await fut
        mydeps[target] = getmtime(target)
        if depsuccess != 0:
            # print("err in dependency of", me, depsuccess)
            err = 1
    if err == 0:
        prev = alldeps.get(me)
        if prev is None:
            alldeps[me] = mydeps
        else:
            # This is a little subtle. The very first time, we clear all
            # existing dependencies, to throw out any stale ones.  But the next
            # time we add to the list of dependencies.
            if me in seen:
                prev |= mydeps
                mydeps = prev
            else:
                alldeps[me] = mydeps
                seen.add(me)
    else:
        print("failed to build dependencies for", me)
    return err



async def monitor():
    global alldeps
    try:
        with open(".deps", "rb") as f:
            alldeps = msgspec.json.decode(f.read())
    except FileNotFoundError:
        alldeps = {}
    err = await redo_ifchange("all", ALL_TARGETS)
    if err != 0:
        return err
    with open(".deps", "wb") as f:
        f.write(msgspec.json.encode(alldeps))
    return err


def main():
    # h = hashlib.sha3_256()
    # a = dis.Bytecode(monitor)
    # h.update(a.codeobj.co_code)
    # print(h.digest())
    os.makedirs("bld/dbg", exist_ok=True)
    os.makedirs("bld/rel", exist_ok=True)
    loop = asyncio.new_event_loop()
    err = loop.run_until_complete(monitor())
    if err != 0:
        return err
    # TODO: run could just be a normal target that redo-ifchanges as necessary
    # and then runs the exe.
    if len(sys.argv) > 1:
        if sys.argv[1] == "run":
            subprocess.run("./mos", shell=False, check=True)

if __name__ == "__main__":
    main()
