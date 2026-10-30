#!/usr/bin/env python3
"""Python 3-compatible equivalent of the Legacy checkdep.py verifier."""

import os
import sys


def read_dep(file):
    with open(file, encoding="utf-8") as stream:
        return [line.rstrip("\n") for line in stream]


def read_sym(file):
    with open(file, encoding="utf-8") as stream:
        return [line.rstrip("\n").split(" ")[1] for line in stream]


def try_solve_deps(file, deps):
    try:
        syms = read_sym(file)
    except (OSError, IndexError):
        return []
    return [symbol for symbol in syms if symbol in deps]


module = sys.argv[1]
modules = sorted(name for name in os.listdir(".") if os.path.isdir(name))
platformdir = "../platform/"
cameras = sorted(
    name for name in os.listdir(platformdir)
    if "." in name
    and name[0] == name[0].upper()
    and "MASTER" not in name
    and os.path.isdir(os.path.join(platformdir, name))
)

deps = read_dep(os.path.join(module, module + ".dep"))
module_deps = []
for name in modules:
    solved = try_solve_deps(os.path.join(name, name + ".sym"), deps)
    if solved:
        module_deps.append((name, solved))
        deps = list(set(deps) - set(solved))

if module_deps:
    print("Depends on modules: ")
    for name, solved in module_deps:
        print("    %s (%s)" % (name, ", ".join(solved)))

working_cameras = []
not_working_cameras = []
not_checked_cameras = []
for camera in cameras:
    camera_name = camera.split(".")[0]
    if camera_name in [name.split(".")[0] for name in cameras if name != camera]:
        camera_name = camera
    camera_sym = os.path.join(platformdir, camera, "magiclantern.sym")
    solved = try_solve_deps(camera_sym, deps)
    if solved or not deps:
        unsolved_deps = list(set(deps) - set(solved))
        if unsolved_deps:
            not_working_cameras.append((camera_name, unsolved_deps))
        else:
            working_cameras.append(camera_name)
    elif os.path.isfile(camera_sym):
        if len(open(camera_sym, encoding="utf-8").read()) == 0:
            not_checked_cameras.append(camera_name + " (empty sym)")
        else:
            print(solved, deps)
            not_checked_cameras.append(camera_name + " (error)")
    else:
        not_checked_cameras.append(camera_name)

if working_cameras:
    print("Will load on:\n   ", ", ".join(working_cameras))
if not_working_cameras:
    print("Will NOT load on: ")
    for camera_name, unsolved_deps in not_working_cameras:
        if len(unsolved_deps) > 4:
            unsolved_deps = unsolved_deps[:3] + ["and %d others" % (len(unsolved_deps) - 3)]
        print("    %s (%s)" % (camera_name, ", ".join(unsolved_deps)))
if not_checked_cameras:
    print("Not checked (compile ML for these cameras first):\n   ", ", ".join(not_checked_cameras))

if not working_cameras:
    try:
        os.remove(os.path.join(module, module + ".dep"))
    except FileNotFoundError:
        pass
    raise SystemExit(1)
