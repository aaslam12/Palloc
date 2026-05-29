#!/usr/bin/env python3
import argparse
import os
import platform
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description="Build, test, and run the PALLOC.")
    parser.add_argument(
        "--config",
        default="Debug",
        choices=["Debug", "Release"],
        help="Build configuration (default: Debug)",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Clean build directory before building",
    )
    parser.add_argument(
        "--build-only",
        action="store_true",
        help="Only build, do not run tests or executable",
    )
    parser.add_argument(
        "--no-tests",
        action="store_true",
        help="Disable building and running tests (default for Release)",
    )
    parser.add_argument(
        "--stress-test", action="store_true", help="Build and run stress tests"
    )
    parser.add_argument(
        "--single-threaded",
        action="store_true",
        help="Build with PALLOC_SINGLE_THREADED (no-op mutexes, non-atomic counters)",
    )
    parser.add_argument(
        "--enable-thp",
        action="store_true",
        help="Build with support for transparent huge pages.",
    )
    parser.add_argument(
        "--use-2mb-huge-pages",
        action="store_true",
        help="Build with support for transparent huge pages.",
    )
    parser.add_argument(
        "--static", action="store_true", help="Link libraries statically"
    )
    parser.add_argument(
        "--install",
        help="Install to the specified prefix (e.g., ~/.local or /usr/local)",
    )
    sanitizer_group = parser.add_mutually_exclusive_group()
    sanitizer_group.add_argument(
        "--asan",
        action="store_true",
        help="Enable AddressSanitizer + UndefinedBehaviorSanitizer (Debug only). "
        "Detects memory errors: use-after-free, buffer overflows, leaks.",
    )
    sanitizer_group.add_argument(
        "--tsan",
        action="store_true",
        help="Enable ThreadSanitizer (Debug only). "
        "Detects data races and concurrency bugs.",
    )

    args = parser.parse_args()

    if (args.asan or args.tsan) and args.config != "Debug":
        print("Error: --asan and --tsan require --config Debug.")
        sys.exit(1)

    sanitizer_suffix = "-asan" if args.asan else "-tsan" if args.tsan else ""

    # --- Path Setup ---
    project_root = os.path.dirname(os.path.abspath(__file__))
    build_dir = os.path.join(project_root, "build", args.config + sanitizer_suffix)

    # Executable name handling for Windows
    executable_names = ["palloc_exe", "palloc"]
    if platform.system() == "Windows":
        executable_names = [name + ".exe" for name in executable_names]

    # --- Clean Step ---
    if args.clean:
        temp_build_dir = os.path.join(project_root, "build")
        print(f"=== Cleaning {temp_build_dir} ===")
        if os.path.exists(temp_build_dir):
            shutil.rmtree(temp_build_dir)
        if os.path.isfile(os.path.join(project_root, "compile_commands.json")):
            os.remove(os.path.join(project_root, "compile_commands.json"))
        sys.exit(0)

    # --- Configuration Step ---
    print(f"=== Configuring ({args.config}) ===")
    os.makedirs(build_dir, exist_ok=True)

    # Determine if tests should be enabled
    # Default: Enable tests only in Debug mode
    build_tests = (args.config == "Debug") and not args.no_tests

    cmake_args = [
        f"-DCMAKE_BUILD_TYPE={args.config}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-DPALLOC_BUILD_TESTS={'ON' if build_tests else 'OFF'}",
        f"-DPALLOC_BUILD_STRESS_TESTS={'ON' if args.stress_test else 'OFF'}",
        f"-DPALLOC_STATIC_LINKING={'ON' if args.static else 'OFF'}",
        f"-DPALLOC_SINGLE_THREADED={'ON' if args.single_threaded else 'OFF'}",
    ]

    if args.asan:
        cmake_args += [
            "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer",
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined",
        ]
    elif args.tsan:
        cmake_args += [
            "-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer",
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread",
        ]

    # Generator selection: Prefer Ninja if available, else let CMake decide
    cmd_config = ["cmake", "-S", project_root, "-B", build_dir]
    if shutil.which("ninja"):
        cmd_config.extend(["-G", "Ninja"])

    cmd_config.extend(cmake_args)
    subprocess.check_call(cmd_config)

    # --- Build Step ---
    print(f"=== Building ({args.config}) ===")
    cpu_count = os.cpu_count() or 1
    subprocess.check_call(["cmake", "--build", build_dir, "--parallel", str(cpu_count)])

    # --- Post-Build ---
    # Symlink compile_commands.json to root for clangd support
    compile_commands_src = os.path.join(build_dir, "compile_commands.json")
    compile_commands_dst = os.path.join(project_root, "compile_commands.json")

    if os.path.exists(compile_commands_src):
        try:
            if os.path.exists(compile_commands_dst) or os.path.islink(
                compile_commands_dst
            ):
                os.remove(compile_commands_dst)

            try:
                os.symlink(compile_commands_src, compile_commands_dst)
            except OSError:
                # Windows fallback: copy if symlink fails (requires admin usually)
                shutil.copy(compile_commands_src, compile_commands_dst)
        except Exception as e:
            print(f"Warning: Could not link compile_commands.json: {e}")

    if args.build_only:
        print("Build complete.")
        return

    # --- Test Step ---
    if build_tests:
        print("\n=== Running Tests ===")
        try:
            # Create a copy of the environment and add color forcing variables
            env = os.environ.copy()
            env["CTEST_COLOR_OUTPUT"] = "ON"
            env["CLICOLOR_FORCE"] = "1"
            if args.tsan:
                env.setdefault("TSAN_OPTIONS", "suppress_equal_stacks=1")
            if args.asan:
                env.setdefault("ASAN_OPTIONS", "halt_on_error=0:detect_leaks=1")

            # ctest handles running the registered tests
            subprocess.check_call(
                ["ctest", "--output-on-failure", "--test-dir", build_dir], env=env
            )

            # Under TSan, also explicitly run the hidden [thread] test suite.
            if args.tsan:
                tests_exe = os.path.join(build_dir, "tests")
                if platform.system() == "Windows":
                    tests_exe += ".exe"
                if os.path.exists(tests_exe):
                    print("\n=== Running thread-safety tests under TSan ([thread]) ===")
                    subprocess.check_call([tests_exe, "[thread]"], env=env)
        except subprocess.CalledProcessError:
            print("Tests failed.")

    # --- Stress Test Step ---
    if args.stress_test:
        print("\n=== Running Stress Tests ===")
        stress_src_dir = os.path.join(project_root, "stress_tests")

        if os.path.isdir(stress_src_dir):
            found_tests = False
            for filename in sorted(os.listdir(stress_src_dir)):
                if filename.endswith(".cpp"):
                    found_tests = True
                    test_name = os.path.splitext(filename)[0]
                    stress_exe = os.path.join(build_dir, test_name)

                    if platform.system() == "Windows":
                        stress_exe += ".exe"

                    if os.path.exists(stress_exe):
                        print(f"--- Running {test_name} ---")
                        try:
                            subprocess.check_call([stress_exe])
                        except subprocess.CalledProcessError:
                            print(f"!!! FAILED: {test_name}")
                    else:
                        print(
                            f"Warning: Executable for {test_name} not found (build might have failed)."
                        )

            if not found_tests:
                print("No .cpp files found in stress_tests/.")
        else:
            print("Directory stress_tests/ does not exist.")

    # --- Run Step ---
    print("\n=== Running Application ===")
    executable_path = next(
        (
            os.path.join(build_dir, executable_name)
            for executable_name in executable_names
            if os.path.exists(os.path.join(build_dir, executable_name))
        ),
        None,
    )
    if executable_path:
        try:
            subprocess.check_call([executable_path])
        except subprocess.CalledProcessError as e:
            sys.exit(e.returncode)
    else:
        expected_paths = ", ".join(os.path.join(build_dir, name) for name in executable_names)
        print(f"Error: Executable not found. Checked: {expected_paths}")
        sys.exit(1)

    # --- Install Step ---
    if args.install:
        print(f"\n=== Installing to {args.install} ===")
        subprocess.check_call(
            ["cmake", "--install", build_dir, "--prefix", args.install]
        )
        print(
            f"Installation complete. Headers in {args.install}/include, library in {args.install}/lib"
        )


if __name__ == "__main__":
    main()
