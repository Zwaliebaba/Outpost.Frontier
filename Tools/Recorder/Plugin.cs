// The Recorder (Design/Plan.md 1.1): per simulation tick of the original game, the commands applied,
// the checksum the simulation computed, the checksum a replay expected, and the scheduler's verdict.
//
// One tick of the original (BBI.Game.Commands.SimScheduler.AdvanceSimFrame) runs, on the sim thread:
//   SimCommandBuffer.ProcessCommandFrame(frame, ref expectedChecksum)
//     -> RetrieveScheduledCommandsForFrame(frame)       live: the commands scheduled for the frame
//     -> RetrieveRecordedCommandsForFrame(frame, out c)  replay: the recorded commands and checksum
//   Sim.Tick(frame) -> Checksum                           the tick itself
//   ... -> SchedulerResultType (OK, SyncError, ...)       SyncError: a replay's checksum differed
// The hooks are postfixes that only observe, except that a replay's recorded commands (a lazy
// sequence) are materialised into a list once, so they can be both logged and executed.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using BBI.Core.Utility;
using BBI.Game.Commands;
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using UnityEngine;

namespace Outpost.Recorder
{
    [BepInPlugin("outpost.frontier.recorder", "Outpost Recorder", "0.1.0")]
    public sealed class Plugin : BaseUnityPlugin
    {
        internal static Plugin Instance;
        internal static string Scene = string.Empty;

        private ConfigEntry<string> m_outputDirectory;
        private ConfigEntry<bool> m_skipGalaxy;

        private void Awake()
        {
            Instance = this;
            m_outputDirectory = Config.Bind("Recorder", "OutputDirectory", Path.Combine(Paths.BepInExRootPath, "Recorder"),
                "Folder for the trace files (one per game session).");
            m_skipGalaxy = Config.Bind("Recorder", "SkipGalaxyLanguageQuery", true,
                "Answer GogAPIIntegration.GalaxyUserLanguage with \"en\" instead of asking GOG Galaxy. Galaxy's " +
                "peer (GalaxyPeer64.dll) crashes on that query when the game runs from anywhere but its registered " +
                "installation folder, as this copy does; in the installation itself the query fails and the game " +
                "falls back to \"en\", so the answer is the same. (Galaxy itself must initialise: without it the " +
                "game quits at startup, in EnableSteam's DLC query.)");
            Trace.OutputDirectory = m_outputDirectory.Value;

            var harmony = new Harmony("outpost.frontier.recorder");
            if (m_skipGalaxy.Value)
            {
                Type gog = AccessTools.TypeByName("GOG.GalaxyWrapper.GogAPIIntegration");
                var language = gog != null ? AccessTools.PropertyGetter(gog, "GalaxyUserLanguage") : null;
                if (language != null)
                {
                    harmony.Patch(language, prefix: new HarmonyMethod(typeof(Hooks), nameof(Hooks.GalaxyLanguage)));
                    Logger.LogInfo("Recorder: GOG Galaxy language query answered locally (\"en\")");
                }
                else
                {
                    Logger.LogWarning("Recorder: GogAPIIntegration.GalaxyUserLanguage not found; not patched");
                }
            }
            Patch(harmony, "BBI.Game.Commands.SimCommandBuffer", "RetrieveScheduledCommandsForFrame", nameof(Hooks.ScheduledCommands));
            Patch(harmony, "BBI.Game.Commands.SimCommandBuffer", "RetrieveRecordedCommandsForFrame", nameof(Hooks.RecordedCommands));
            Patch(harmony, "BBI.Game.Commands.SimCommandBuffer", "ProcessCommandFrame", nameof(Hooks.ProcessCommandFrame));
            Patch(harmony, "BBI.Game.Simulation.Sim", "Tick", nameof(Hooks.SimTick));
            Patch(harmony, "BBI.Game.Commands.SimScheduler", "AdvanceSimFrame", nameof(Hooks.AdvanceSimFrame));
            Logger.LogInfo("Recorder ready; traces go to " + Trace.OutputDirectory);
        }

        private void Patch(Harmony _harmony, string _typeName, string _method, string _postfix)
        {
            Type type = AccessTools.TypeByName(_typeName);
            var original = type != null ? AccessTools.Method(type, _method) : null;
            if (original == null)
            {
                Logger.LogError("Recorder: " + _typeName + "." + _method + " not found; that hook is off");
                return;
            }
            _harmony.Patch(original, postfix: new HarmonyMethod(typeof(Hooks), _postfix));
            Logger.LogInfo("Recorder: hooked " + _typeName + "." + _method);
        }

        // Unity's API is main-thread only; the sim runs on its own thread, so the scene name is
        // sampled here and read there.
        private void Update()
        {
            Scene = Application.loadedLevelName ?? string.Empty;
        }

        private void OnApplicationQuit()
        {
            Trace.Close("application quit");
        }

        internal static void Log(string _text)
        {
            if (Instance != null)
                Instance.Logger.LogInfo(_text);
        }
    }

    internal static class Hooks
    {
        // A prefix returning false skips the original property getter and returns __result.
        public static bool GalaxyLanguage(ref string __result)
        {
            __result = "en";
            return false;
        }

        public static void ScheduledCommands(IDictionary<int, SimCommandBase> __result)
        {
            Trace.Commands("live", __result != null ? __result.Values : null);
        }

        public static void RecordedCommands(ref IEnumerable<SimCommandBase> __result)
        {
            if (__result != null)
                __result = new List<SimCommandBase>(__result);
            Trace.Commands("replay", __result);
        }

        public static void ProcessCommandFrame(ref Checksum expectedChecksum)
        {
            Trace.Expected(expectedChecksum);
        }

        public static void SimTick(SimFrameNumber frameNumber, Checksum __result)
        {
            Trace.Tick(frameNumber, __result);
        }

        public static void AdvanceSimFrame(SchedulerResultType __result)
        {
            Trace.EndFrame(__result);
        }
    }

    // Trace format (UTF-8, tab-separated, one line per tick, '#' lines are metadata):
    //   frame  checksum  expected  result  mode  commands
    // checksum and expected are Checksum.Sum (expected is '-' when the frame carried none);
    // commands are 'Type:checksum' (SimCommandBase.GetChecksum()) joined by ';', '-' for none.
    internal static class Trace
    {
        internal static string OutputDirectory;

        private static readonly object sLock = new object();
        private static StreamWriter sWriter;
        private static uint sLastFrame;
        private static string sMode = "live";
        private static readonly List<string> sCommands = new List<string>();
        private static string sExpected = "-";
        private static bool sTicked;
        private static uint sFrame;
        private static int sChecksum;

        internal static void Commands(string _mode, IEnumerable<SimCommandBase> _commands)
        {
            lock (sLock)
            {
                sMode = _mode;
                sCommands.Clear();
                if (_commands == null)
                    return;
                foreach (SimCommandBase command in _commands)
                    sCommands.Add(command.GetType().Name + ":" + command.GetChecksum());
            }
        }

        internal static void Expected(Checksum _expected)
        {
            lock (sLock)
                sExpected = _expected != null ? _expected.Sum.ToString() : "-";
        }

        internal static void Tick(SimFrameNumber _frame, Checksum _checksum)
        {
            lock (sLock)
            {
                sFrame = _frame != null ? _frame.FrameNumber : 0u;
                sChecksum = _checksum != null ? _checksum.Sum : 0;
                sTicked = true;
            }
        }

        internal static void EndFrame(SchedulerResultType _result)
        {
            lock (sLock)
            {
                if (!sTicked)
                    return;
                sTicked = false;
                try
                {
                    if (sWriter == null || sFrame <= sLastFrame)
                        Open();
                    sLastFrame = sFrame;
                    var line = new StringBuilder();
                    line.Append(sFrame).Append('\t').Append(sChecksum).Append('\t').Append(sExpected).Append('\t')
                        .Append(_result).Append('\t').Append(sMode).Append('\t')
                        .Append(sCommands.Count == 0 ? "-" : string.Join(";", sCommands.ToArray()));
                    sWriter.WriteLine(line.ToString());
                    if (_result == SchedulerResultType.SyncError)
                        Plugin.Log("Recorder: SyncError at frame " + sFrame + " (computed " + sChecksum + ", expected " + sExpected + ")");
                }
                catch (Exception e)
                {
                    Plugin.Log("Recorder: trace write failed: " + e.Message);
                }
                finally
                {
                    sCommands.Clear();
                    sExpected = "-";
                }
            }
        }

        private static void Open()
        {
            Close("new session");
            Directory.CreateDirectory(OutputDirectory);
            string path = Path.Combine(OutputDirectory, DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + sMode + ".tsv");
            sWriter = new StreamWriter(path, false, new UTF8Encoding(false)) { AutoFlush = true };
            sWriter.WriteLine("# Outpost Recorder 0.1.0 trace");
            sWriter.WriteLine("# started " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + ", mode " + sMode + ", scene " + Plugin.Scene);
            sWriter.WriteLine("# frame\tchecksum\texpected\tresult\tmode\tcommands");
            sLastFrame = 0;
            Plugin.Log("Recorder: new trace " + path);
        }

        internal static void Close(string _why)
        {
            lock (sLock)
            {
                if (sWriter == null)
                    return;
                sWriter.WriteLine("# closed: " + _why);
                sWriter.Close();
                sWriter = null;
            }
        }
    }
}
