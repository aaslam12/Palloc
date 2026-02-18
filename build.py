#!/usr/bin/env python3
import argparse
import os
import platform
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(
        description="Build, test, and run the SLABALLOCATOR."
    )
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
        "--static", action="store_true", help="Link libraries statically"
    )

    args = parser.parse_args()

    # --- Path Setup ---
    project_root = os.path.dirname(os.path.abspath(__file__))
    build_dir = os.path.join(project_root, "build", args.config)

    # Executable name handling for Windows
    executable_name = "slaballocator"
    if platform.system() == "Windows":
        executable_name += ".exe"
    executable_path = os.path.join(build_dir, executable_name)

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
        f"-DSLABALLOCATOR_BUILD_TESTS={'ON' if build_tests else 'OFF'}",
        f"-DSLABALLOCATOR_BUILD_STRESS_TESTS={'ON' if args.stress_test else 'OFF'}",
        f"-DSLABALLOCATOR_STATIC_LINKING={'ON' if args.static else 'OFF'}",
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

            # ctest handles running the registered tests
            subprocess.check_call(
                ["ctest", "--output-on-failure", "--test-dir", build_dir], env=env
            )
        except subprocess.CalledProcessError:
            print("Tests failed.")
            # We don't exit here to allow running the app if desired,
            # mirroring the '|| true' behavior of the original script.

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
    if os.path.exists(executable_path):
        try:
            subprocess.check_call([executable_path])
        except subprocess.CalledProcessError as e:
            sys.exit(e.returncode)
    else:
        print(f"Error: Executable not found at {executable_path}")
        sys.exit(1)


if __name__ == "__main__":
    main()
