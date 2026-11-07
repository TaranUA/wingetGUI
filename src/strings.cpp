// Forces libstdc++'s std::string / std::wstring members to be instantiated in
// this translation unit.  The precompiled libstdc++.a objects put every member
// function of a string instantiation in one section, so referencing any of
// them pulls in the whole class (~50 KB).  Instantiating them here with
// -ffunction-sections lets the linker keep only the members actually used.
#include <string>

template class std::basic_string<char>;
template class std::basic_string<wchar_t>;