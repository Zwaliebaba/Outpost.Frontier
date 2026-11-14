# Tools/Vectors: test vectors from the original's own code

`Generate.ps1` compiles the decompiled `Fixed64.cs` and `Checksum.cs` (from `decompiledDir` in
`local.json`; never copied here) into a small .NET Framework program and writes
`Source/Tests/Vectors/Fixed64.tsv`: 28,319 cases over 91 operations, from a fixed-seed generator
plus exhaustive edge cases (special values, exact halves, every pair of special values for the
binary operations). `Source/Tests/Fixed64Tests.cpp` checks the C++ `Core::Fixed64` and
`Core::Checksum` against every one of them. The file holds numbers only (Decisions D14).

**.NET Framework, not .NET 10:** since .NET 9, out-of-range float-to-integer conversions saturate;
the game's Mono on x64 returns the integer's minimum (what `cvttsd2si` gives), and so does the .NET
Framework x64 JIT. A later check runs the same vectors inside the game, through the Recorder.

```powershell
pwsh Tools\Vectors\Generate.ps1
```
