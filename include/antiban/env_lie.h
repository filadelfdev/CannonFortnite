#pragma once

namespace cf::ab::envlie {

// Hooks that make the environment look clean to any EAC probe.
//
// 1. NtQuerySystemInformation(SystemCodeIntegrityInformation)
//      → scrubs the TESTSIGNING_ON bit before returning
// 2. NtQuerySystemInformation(SystemModuleInformation)
//      → removes any kernel module whose name hashes to CfRead/CfSpoof
// 3. NtEnumerateKey / NtEnumerateValueKey on Services\*
//      → hides CfRead and CfSpoof service entries
// 4. NtQuerySystemInformation(SystemBootEnvironmentInformation)
//      → masks boot-config flags set by `bcdedit /set testsigning on`
//
// Every hook is an in-line jmp trampoline + preserved original stub,
// installed at process start so EAC never sees the real values.

void InstallAll();
void RemoveAll();

bool Armed();

} // namespace cf::ab::envlie
