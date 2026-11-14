from pathlib import Path
import html
import argparse
import subprocess
import sys
import tempfile
from update_core_server import msbuild

ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / 'Protocol'

CPP = r'''#include "packets.pb.h"
#include <iostream>
#include <string>
int main(int argc, char** argv) {
  multiplayer::protocol::EchoTest m;
  if (argc == 1) {
    m.set_text("cpp-to-csharp");
    const auto b = m.SerializeAsString();
    const char* h = "0123456789abcdef";
    for (unsigned char c : b) std::cout << h[c >> 4] << h[c & 15];
  } else {
    std::string b, s = argv[1];
    for (size_t i = 0; i + 1 < s.size(); i += 2) b += static_cast<char>(std::stoi(s.substr(i, 2), nullptr, 16));
    if (!m.ParseFromString(b)) return 2;
    std::cout << m.text();
  }
}'''
CS = r'''using Google.Protobuf;
using Multiplayer.Protocol;
if (args.Length == 0) Console.Write(Convert.ToHexString(new EchoTest { Text = "csharp-to-cpp" }.ToByteArray()));
else Console.Write(EchoTest.Parser.ParseFrom(Convert.FromHexString(args[0])).Text);'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('configuration', nargs='?', default='Debug', choices=['Debug', 'Release'])
    config = parser.parse_args().configuration
    if not (PROTOCOL / 'build' / config / 'GameProtocol.lib').exists():
        raise RuntimeError(f'Run Build_Protocol.bat {config} first')
    with tempfile.TemporaryDirectory(prefix='protocol_test_', dir=PROTOCOL) as temp:
        p = Path(temp)
        (p / 'main.cpp').write_text(CPP, encoding='utf-8')
        (p / 'Program.cs').write_text(CS, encoding='utf-8')
        props = html.escape(str(PROTOCOL / 'ServerProtocol.props'), quote=True)
        vc = f'''<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations"><ProjectConfiguration Include="{config}|x64"><Configuration>{config}</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup>
  <PropertyGroup Label="Globals"><ProjectGuid>{{70570AF4-392A-4F24-91DB-F8BB659BDAB0}}</ProjectGuid><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration"><ConfigurationType>Application</ConfigurationType><PlatformToolset>v145</PlatformToolset><UseDebugLibraries>{str(config == 'Debug').lower()}</UseDebugLibraries></PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <ItemDefinitionGroup><ClCompile><LanguageStandard>stdcpplatest</LanguageStandard><RuntimeLibrary>{'MultiThreadedDebugDLL' if config == 'Debug' else 'MultiThreadedDLL'}</RuntimeLibrary><AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions></ClCompile><Link><SubSystem>Console</SubSystem></Link></ItemDefinitionGroup>
  <ItemGroup><ClCompile Include="main.cpp" /></ItemGroup>
  <Import Project="{props}" /><Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
</Project>'''
        (p / 'Interop.vcxproj').write_text(vc, encoding='utf-8')
        generated = html.escape(str(PROTOCOL / 'Generated/CSharp/**/*.cs'), quote=True)
        (p / 'Interop.csproj').write_text(f'''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework><ImplicitUsings>enable</ImplicitUsings></PropertyGroup><ItemGroup><PackageReference Include="Google.Protobuf" Version="3.33.6" /><Compile Include="{generated}" /></ItemGroup></Project>''', encoding='utf-8')
        subprocess.run([str(msbuild()), str(p / 'Interop.vcxproj'), '/p:Configuration=' + config, '/p:Platform=x64', '/nologo', '/v:minimal'], cwd=p, check=True)
        subprocess.run(['dotnet', 'build', str(p / 'Interop.csproj'), '-c', config, '--nologo', '-v:q'], cwd=p, check=True)
        exe = p / 'x64' / config / 'Interop.exe'
        dll = p / 'bin' / config / 'net10.0/Interop.dll'
        cpp_hex = subprocess.check_output([str(exe)], text=True)
        cs_read = subprocess.check_output(['dotnet', str(dll), cpp_hex], text=True)
        cs_hex = subprocess.check_output(['dotnet', str(dll)], text=True)
        cpp_read = subprocess.check_output([str(exe), cs_hex], text=True)
        if cs_read != 'cpp-to-csharp' or cpp_read != 'csharp-to-cpp':
            raise RuntimeError(f'Interop mismatch: {cs_read!r}, {cpp_read!r}')
        print('PASS: C++ -> C# and C# -> C++ serialization', flush=True)

if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(1)
