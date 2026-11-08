// Forces libstdc++'s std::string / std::wstring members to be instantiated in
// this translation unit.  The precompiled libstdc++.a objects put every member
// function of a string instantiation in one section, so referencing any of
// them pulls in the whole class (~50 KB).  Instantiating them here with
// -ffunction-sections lets the linker keep only the members actually used.
#include <string>

template class std::basic_string<char>;
template class std::basic_string<wchar_t>;

// Under -fno-exceptions, standard container methods (string, vector) still
// reference libstdc++'s __throw_* entry points for boundary checks.  By default,
// libstdc++.a(functexcept.o) defines these by constructing exception objects,
// which pulls in cow-stdexcept.o, cow-string-inst.o, random.o, system_error.o,
// and the SEH exception unwinder.  Providing no-op traps here lets the linker
// discard that unused runtime machinery completely (~45 KB savings).
namespace std {
    void __throw_bad_alloc() { __builtin_trap(); }
    void __throw_bad_array_new_length() { __builtin_trap(); }
    void __throw_bad_cast() { __builtin_trap(); }
    void __throw_bad_typeid() { __builtin_trap(); }
    void __throw_bad_exception() { __builtin_trap(); }
    void __throw_logic_error(const char*) { __builtin_trap(); }
    void __throw_domain_error(const char*) { __builtin_trap(); }
    void __throw_invalid_argument(const char*) { __builtin_trap(); }
    void __throw_length_error(const char*) { __builtin_trap(); }
    void __throw_out_of_range(const char*) { __builtin_trap(); }
    void __throw_out_of_range_fmt(const char*, ...) { __builtin_trap(); }
    void __throw_runtime_error(const char*) { __builtin_trap(); }
    void __throw_range_error(const char*) { __builtin_trap(); }
    void __throw_overflow_error(const char*) { __builtin_trap(); }
    void __throw_underflow_error(const char*) { __builtin_trap(); }
    void __throw_system_error(int) { __builtin_trap(); }
}