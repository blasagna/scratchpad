//! Compiles the C++ half of this crate.
//!
//! Bazel builds `../cpp` for its own tests and CLI; cargo cannot see Bazel, so
//! it compiles the same two sources itself, plus the shim and the C++ that
//! cxx-build generates from the `#[cxx::bridge]` module in `src/lib.rs`. The
//! sources are shared, the object files are not -- there is one build per build
//! system, and neither has to know about the other.

fn main() {
    cxx_build::bridge("src/lib.rs")
        .file("src/shim.cpp")
        .file("../cpp/exprkit.cpp")
        // `src` resolves include!("shim.hpp") from the generated bridge code;
        // `../cpp` resolves shim.hpp's own #include "exprkit.hpp".
        .include("src")
        .include("../cpp")
        // The library uses std::format and std::numbers. Bazel gets C++20 from
        // --cxxopt in //.bazelrc; this is the same setting for the cargo build.
        .std("c++20")
        .compile("exprkit_bridge");

    // Printing any rerun-if-changed disables cargo's default "watch the whole
    // crate directory" behavior, so every input has to be listed -- including
    // the two that live outside this crate and would not have been watched
    // either way.
    for path in [
        "src/lib.rs",
        "src/shim.hpp",
        "src/shim.cpp",
        "../cpp/exprkit.hpp",
        "../cpp/exprkit.cpp",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }
}
