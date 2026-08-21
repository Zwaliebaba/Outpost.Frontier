@echo off
setlocal EnableExtensions

rem ===========================================================================
rem  GameData beside the executable, so that a build produces something that
rem  runs. Called from Outpost.vcxproj's post-build event, once per
rem  configuration, with two arguments: the content folder and the output
rem  folder.
rem
rem      CopyGameData.cmd [repo]\GameData [repo]\x64\Release\
rem
rem  WHY THE COPY EXISTS
rem
rem  The layout was always implied and never produced. `ResolveContentPath`
rem  tries the working directory and then the executable's own folder
rem  (ConfigLoad.cpp), `LoadAppConfig` looks for Outpost.json the same two
rem  places, and `FileSys::SetHomeDirectory` names a GameData\ folder beside
rem  the executable outright -- but nothing in the build put a single file
rem  there. The copy under x64\[config]\ was maintained by hand, which is a
rem  trap for the next person to add content and find it missing at runtime,
rem  and a fresh clone pressing F5 never had it at all: the debugger launches
rem  with the working directory set to the project folder, where there is no
rem  Outpost.json either.
rem
rem  Outpost.json is lifted out of GameData\ rather than copied inside it,
rem  because beside the executable is the only path the loader reads it from. A
rem  second copy under GameData\ would be inert, and an inert config file is
rem  something to edit by mistake. In the output folder it is build output: an
rem  edit made there is overwritten by the next build, and the two places an
rem  edit survives are the tracked GameData\Outpost.json and the LocalAppData
rem  user layer (ADR-012 section 5).
rem
rem  WHY ROBOCOPY, AND WHAT IT IS ASKED FOR
rem
rem  Skipping unchanged files is what keeps this cheap: the bake alone is ~19
rem  MB, so a copy that did not skip would move the whole universe on every
rem  incremental build. Robocopy skips same-size same-timestamp files by
rem  default, which is the property MSBuild's Copy task spelled as
rem  SkipUnchangedFiles.
rem
rem  /E and not /MIR: this copies, it does not mirror. Content deleted from
rem  GameData\ is left behind in an old output folder, which is accepted over a
rem  mirror that is one wrong path away from deleting a tree.
rem
rem  /XF takes patterns and paths directly, which is the whole reason robocopy
rem  is here rather than xcopy -- see the post-mortem below. A stale log is not
rem  content, so *.log never travels; Outpost.json is excluded by full path so
rem  that only the one at the top of GameData\ is lifted out, exactly as the
rem  MSBuild item's Exclude did.
rem
rem  The per-file and per-directory listings are suppressed and the job summary
rem  is not. A build log wants the tally -- copied, skipped, FAILED -- and not
rem  a hundred and forty file names; and the tally is the line that makes a
rem  copy which quietly did nothing obvious at a glance.
rem
rem  WHY THE EXIT CODE IS TRANSLATED
rem
rem  Robocopy does not report success as 0. It returns a bitmask: 1 means files
rem  were copied, 2 means extras were found in the destination, 3 means both,
rem  and everything below 8 is a healthy run. 8 and above is a real failure.
rem  A caller that treats non-zero as failure therefore fails the build the
rem  first time the copy has anything to do -- so the comparison here is
rem  GEQ 8, and the script exits 0 or 1 so that MSBuild sees an ordinary
rem  answer.
rem
rem  THE POST-MORTEM THIS SCRIPT IS SHAPED BY
rem
rem  An xcopy rewrite of this shipped with two defects that a stale local
rem  output tree hid, and cost two 45-minute CI jobs to find. xcopy's
rem  /EXCLUDE: names a file that lists patterns, not a pattern, so the copy
rem  aborted having copied nothing; the Debug variant dropped the GameData\
rem  prefix, so content landed where the loader never looks; and because a
rem  multi-line post-build event reports only the last command's exit code, the
rem  failure was invisible and the build stayed green. On CI's fresh clone the
rem  executable then booted without its content and blocked on a modal error
rem  box.
rem
rem  Every one of those is answered structurally rather than by being careful:
rem  one script instead of a command per configuration, so the destination is
rem  written once; /XF instead of /EXCLUDE:, so a pattern is a pattern; a
rem  single command in the event and an explicit exit /b, so the exit code is
rem  the script's and not the last echo's; and, because none of that proves the
rem  files arrived, a CI step after the build asserts the layout by name.
rem
rem  Messages are written in MSBuild's canonical error format
rem  (`origin : error : text`), so a failure here is an error in the build log
rem  and in the Error List rather than a line of console output to scroll past.
rem ===========================================================================

set "SRC=%~1"
set "DEST=%~2"

if not defined SRC goto :usage
if not defined DEST goto :usage

rem  Neither path may end in a backslash. That sentence has now cost a build,
rem  so it is worth the paragraph.
rem
rem  cmd has no backslash escape, so a quoted path ending in one looks perfectly
rem  balanced to the batch parser and raises nothing here. The program on the
rem  other end is where it goes wrong: robocopy parses its own command line with
rem  the C runtime's rules, and there a backslash before a quote IS an escape.
rem  The closing quote stops closing anything, the switches after it are
rem  swallowed into the path, and robocopy reports a destination directory with
rem  the rest of the command line inside it -- run 162, exit code 16:
rem
rem      ERROR 123 Accessing Destination Directory D:\...\Release  Outpost.json /NJH ...
rem
rem  So it is stripped here, once, for both arguments -- MSBuild's $(OutDir) and
rem  $(TargetDir) always carry one -- and then checked rather than assumed. The
rem  check costs two lines and turns a future regression of the strip into a
rem  named error instead of a mangled command line for somebody to decode.
rem
rem  By comparison and not by %%~f, which is the actual lesson of run 162: %%~f
rem  fully qualifies a path but keeps a trailing backslash exactly as it was
rem  given. These two lines were correct, were replaced by a for loop over %%~f
rem  on the belief that it trimmed as well, and were wrong the moment they built.
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
if "%DEST:~-1%"=="\" set "DEST=%DEST:~0,-1%"

if "%SRC:~-1%"=="\" goto :trailingSlash
if "%DEST:~-1%"=="\" goto :trailingSlash

if not exist "%SRC%\." goto :noSource

echo CopyGameData: "%SRC%" into "%DEST%\GameData", with Outpost.json beside the executable

rem  Both excludes are names and not paths. A path would have to agree with
rem  whatever robocopy canonicalised the source to -- the project passes its
rem  content folder as a ..\ walk out of the project directory -- and a
rem  mismatch there fails open: Outpost.json lands inside GameData\ after
rem  all, which is the one thing this script exists to prevent. A name
rem  cannot mismatch. It also excludes a nested Outpost.json, which the
rem  MSBuild item did not, and that is the better behaviour rather than a
rem  concession: a config file anywhere but beside the executable is inert,
rem  and shipping an inert one is what the paragraph above is about.
robocopy "%SRC%" "%DEST%\GameData" /E /XF *.log Outpost.json /NJH /NFL /NDL /NP
set "RC=%ERRORLEVEL%"
if %RC% GEQ 8 goto :copyFailed

rem  And then the one file, by name, as robocopy's file-set rather than as an
rem  exclude: no recursion, so only the copy at the top of the content
rem  folder is lifted out, which is the file the loader reads.
robocopy "%SRC%" "%DEST%" Outpost.json /NJH /NFL /NDL /NP
set "RC=%ERRORLEVEL%"
if %RC% GEQ 8 goto :copyFailed

rem  Cheap proof that the two destinations were reached at all. It is not a
rem  content check -- naming files here would put the content layout in a build
rem  script -- and the CI step that follows the build does name them.
if not exist "%DEST%\GameData\." goto :noTree
if not exist "%DEST%\Outpost.json" goto :noConfig

exit /b 0

:usage
echo CopyGameData.cmd : error : usage: CopyGameData.cmd [content folder] [output folder]
exit /b 1

:noSource
echo CopyGameData.cmd : error : there is no content folder at "%SRC%"
exit /b 1

:trailingSlash
echo CopyGameData.cmd : error : a path still ends in a backslash: "%SRC%" and "%DEST%"
exit /b 1

:copyFailed
echo CopyGameData.cmd : error : robocopy exited %RC% -- 8 and above is a real failure
exit /b 1

:noTree
echo CopyGameData.cmd : error : "%DEST%\GameData" was not created
exit /b 1

:noConfig
echo CopyGameData.cmd : error : Outpost.json did not land in "%DEST%"
exit /b 1
