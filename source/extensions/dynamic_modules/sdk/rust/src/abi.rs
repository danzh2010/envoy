/* automatically generated by rust-bindgen 0.70.1 */

pub type wchar_t = ::std::os::raw::c_int;
#[repr(C)]
#[repr(align(16))]
#[derive(Debug, Copy, Clone)]
pub struct max_align_t {
    pub __clang_max_align_nonce1: ::std::os::raw::c_longlong,
    pub __bindgen_padding_0: u64,
    pub __clang_max_align_nonce2: u128,
}
#[doc = " envoy_dynamic_module_type_abi_version represents a null-terminated string that contains the ABI\n version of the dynamic module. This is used to ensure that the dynamic module is built against\n the compatible version of the ABI."]
pub type envoy_dynamic_module_type_abi_version = *const ::std::os::raw::c_char;
extern "C" {
    #[doc = " envoy_dynamic_module_on_program_init is called by the main thread exactly when the module is\n loaded. The function returns the ABI version of the dynamic module. If null is returned, the\n module will be unloaded immediately.\n\n For Envoy, the return value will be used to check the compatibility of the dynamic module.\n\n For dynamic modules, this is useful when they need to perform some process-wide\n initialization or check if the module is compatible with the platform, such as CPU features.\n Note that initialization routines of a dynamic module can also be performed without this function\n through constructor functions in an object file. However, normal constructors cannot be used\n to check compatibility and gracefully fail the initialization because there is no way to\n report an error to Envoy.\n\n @return envoy_dynamic_module_type_abi_version is the ABI version of the dynamic module. Null\n         means the error and the module will be unloaded immediately."]
    pub fn envoy_dynamic_module_on_program_init() -> envoy_dynamic_module_type_abi_version;
}
