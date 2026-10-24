fn main() {
    // libsteam_api.so travels next to the binary
    println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN");
}
