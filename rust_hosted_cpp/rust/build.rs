//! Compiles the C++ half of this crate -- which is to say, all of the C++.
//!
//! `../cpp` has no build file of its own. This script is the only place that
//! knows those sources exist, which standard they need, and which warnings they
//! are held to. It hands them to `cxx_build` alongside the shim and the C++ that
//! cxx generates from the `#[cxx::bridge]` module in `src/lib.rs`, and the
//! result is linked straight into the crate.
//!
//! Contrast `../../cpp_rust_bindings/rust/build.rs`, which does the same thing
//! for a library Bazel *also* builds. There, this script is a second opinion and
//! the two can disagree; here it is the only opinion, so anything the C++ needs
//! has to be stated below or it does not happen at all.

use std::env;

fn main() {
    let mut build = cxx_build::bridge("src/lib.rs");

    build
        .file("src/shim.cpp")
        .file("../cpp/lrukit.cpp")
        // `src` resolves include!("shim.hpp") from the generated bridge code;
        // `../cpp` resolves shim.hpp's own #include "lrukit.hpp". The generated
        // header that shim.hpp includes back needs no entry: cxx_build puts its
        // directory on the include path itself.
        .include("src")
        .include("../cpp")
        // lrukit.cpp uses std::format. There is no //.bazelrc here to set the
        // standard, so this line is the standard.
        .std("c++20")
        // Nor is there a .bazelrc to make warnings fatal. The repo builds C++
        // with -Wall -Werror -Wextra -pedantic by default and offers
        // `--config=permissive` to escape it; these three calls are that policy
        // restated for the one build system this area has, escape hatch
        // included. Dropping them would leave this the only C++ in the repo
        // compiled with warnings off.
        .warnings(true)
        .extra_warnings(true)
        .flag_if_supported("-pedantic");

    // The generated bridge code is compiled by the same cc::Build as the
    // hand-written sources, so -Werror applies to it too. That is fine today
    // and is exactly the sort of thing a cxx upgrade can break, which is what
    // the escape hatch is for: LRUKIT_PERMISSIVE=1 keeps the warnings visible
    // without making them fatal.
    let permissive = env::var_os("LRUKIT_PERMISSIVE").is_some();
    build.warnings_into_errors(!permissive);

    build.compile("lrukit_bridge");

    // Printing any rerun-if-changed disables cargo's default "watch the whole
    // crate directory" behavior, so every input has to be listed -- including
    // the two that live outside this crate and would not have been watched
    // either way.
    for path in [
        "src/lib.rs",
        "src/shim.hpp",
        "src/shim.cpp",
        "../cpp/lrukit.hpp",
        "../cpp/lrukit.cpp",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }
    println!("cargo:rerun-if-env-changed=LRUKIT_PERMISSIVE");
}
