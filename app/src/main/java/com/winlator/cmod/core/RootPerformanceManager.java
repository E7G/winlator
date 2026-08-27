package com.winlator.cmod.core;

import android.util.Log;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/**
 * Optional, reversible root-assisted performance tuning.
 *
 * This deliberately does NOT disable thermal protection, charging protection,
 * SELinux, or other safety mechanisms. It only boosts CPU/GPU frequency policy
 * while a Winlator session is active and restores previous sysfs values on exit.
 */
public final class RootPerformanceManager {
    private static final String TAG = "RootPerformance";
    private static final ExecutorService EXECUTOR = Executors.newSingleThreadExecutor();
    private static volatile boolean active;

    private RootPerformanceManager() {}

    private static String statePath() {
        return "/data/local/tmp/winlator-e7g-rootperf-" + android.os.Process.myUid() + ".state";
    }

    public static boolean isActive() {
        return active;
    }

    public static void recoverStaleStateAsync() {
        EXECUTOR.execute(() -> {
            runRoot(buildRestoreScript(), 12);
            active = false;
        });
    }

    public static void applyExtremeModeAsync(int appPid) {
        EXECUTOR.execute(() -> {
            RootResult result = runRoot(buildApplyScript(appPid), 20);
            active = result.ok;
            Log.i(TAG, "applyExtremeMode: root=" + result.ok + " output=" + result.output);
        });
    }

    public static void boostProcessTreeAsync(int rootPid) {
        if (rootPid <= 0) return;
        EXECUTOR.execute(() -> {
            RootResult result = runRoot(buildProcessBoostScript(rootPid), 10);
            Log.d(TAG, "boostProcessTree(" + rootPid + "): " + result.ok);
        });
    }

    public static void restoreAsync() {
        EXECUTOR.execute(() -> {
            RootResult result = runRoot(buildRestoreScript(), 15);
            active = false;
            Log.i(TAG, "restore: " + result.ok);
        });
    }

    public static boolean checkRoot(int timeoutSeconds) {
        RootResult result = runRoot("id -u", Math.max(2, timeoutSeconds));
        return result.ok && result.output.trim().equals("0");
    }

    private static String buildRestoreScript() {
        String state = statePath();
        return "STATE='" + state + "'; " +
                "if [ -s \"$STATE\" ]; then sh \"$STATE\" >/dev/null 2>&1 || true; fi; " +
                "rm -f \"$STATE\"; exit 0";
    }

    private static String buildApplyScript(int appPid) {
        String state = statePath();
        return ""
                + "STATE='" + state + "'\n"
                + "if [ -s \"$STATE\" ]; then sh \"$STATE\" >/dev/null 2>&1 || true; fi\n"
                + "rm -f \"$STATE\"; : > \"$STATE\"; chmod 600 \"$STATE\" 2>/dev/null || true\n"
                + "save_knob() {\n"
                + "  f=\"$1\"; v=\"$2\"\n"
                + "  [ -f \"$f\" ] || return 0\n"
                + "  old=$(cat \"$f\" 2>/dev/null | tr -d '\\r\\n')\n"
                + "  [ -n \"$old\" ] || return 0\n"
                + "  printf \"printf '%%s' '%s' > '%s' 2>/dev/null || true\\n\" \"$old\" \"$f\" >> \"$STATE\"\n"
                + "  printf '%s' \"$v\" > \"$f\" 2>/dev/null || true\n"
                + "}\n"
                + "# CPU: performance governor when available + pin minimum to current maximum.\n"
                + "for p in /sys/devices/system/cpu/cpufreq/policy*; do\n"
                + "  [ -d \"$p\" ] || continue\n"
                + "  if [ -f \"$p/scaling_available_governors\" ] && grep -qw performance \"$p/scaling_available_governors\" 2>/dev/null; then\n"
                + "    save_knob \"$p/scaling_governor\" performance\n"
                + "  fi\n"
                + "  max=$(cat \"$p/scaling_max_freq\" 2>/dev/null)\n"
                + "  [ -n \"$max\" ] && save_knob \"$p/scaling_min_freq\" \"$max\"\n"
                + "done\n"
                + "# Qualcomm/standard GPU devfreq: keep the GPU at its configured max clock.\n"
                + "for d in /sys/class/kgsl/kgsl-3d0/devfreq /sys/class/devfreq/*; do\n"
                + "  [ -d \"$d\" ] || continue\n"
                + "  tag=$(echo \"$d $(readlink -f \"$d\" 2>/dev/null)\" | tr '[:upper:]' '[:lower:]')\n"
                + "  case \"$tag\" in *kgsl*|*adreno*|*gpu*) ;; *) continue ;; esac\n"
                + "  if [ -f \"$d/available_governors\" ] && grep -qw performance \"$d/available_governors\" 2>/dev/null; then\n"
                + "    save_knob \"$d/governor\" performance\n"
                + "  fi\n"
                + "  gmax=$(cat \"$d/max_freq\" 2>/dev/null)\n"
                + "  [ -n \"$gmax\" ] && save_knob \"$d/min_freq\" \"$gmax\"\n"
                + "done\n"
                + "# Qualcomm KGSL optional keep-awake knobs. Thermal management stays enabled.\n"
                + "for k in force_clk_on force_bus_on force_rail_on; do\n"
                + "  [ -f \"/sys/class/kgsl/kgsl-3d0/$k\" ] && save_knob \"/sys/class/kgsl/kgsl-3d0/$k\" 1\n"
                + "done\n"
                + "# Prioritize the Android process itself. Child Wine/Box64/FEX processes are tuned separately.\n"
                + "renice -n -20 -p " + appPid + " >/dev/null 2>&1 || true\n"
                + "ionice -c 1 -n 0 -p " + appPid + " >/dev/null 2>&1 || true\n"
                + "echo ROOT_PERF_APPLIED\n";
    }

    private static String buildProcessBoostScript(int rootPid) {
        return ""
                + "tune_tree() {\n"
                + "  p=\"$1\"; [ -d \"/proc/$p\" ] || return\n"
                + "  renice -n -20 -p \"$p\" >/dev/null 2>&1 || true\n"
                + "  ionice -c 1 -n 0 -p \"$p\" >/dev/null 2>&1 || true\n"
                + "  children=$(cat \"/proc/$p/task/$p/children\" 2>/dev/null)\n"
                + "  for c in $children; do tune_tree \"$c\"; done\n"
                + "}\n"
                + "tune_tree " + rootPid + "\n"
                + "echo PROCESS_TREE_BOOSTED\n";
    }

    private static RootResult runRoot(String command, int timeoutSeconds) {
        java.lang.Process proc = null;
        StringBuilder out = new StringBuilder();
        try {
            proc = new ProcessBuilder("su", "-c", command)
                    .redirectErrorStream(true)
                    .start();
            BufferedReader reader = new BufferedReader(new InputStreamReader(proc.getInputStream()));
            Thread drain = new Thread(() -> {
                try {
                    String line;
                    while ((line = reader.readLine()) != null) {
                        synchronized (out) {
                            if (out.length() < 8192) out.append(line).append('\n');
                        }
                    }
                } catch (Exception ignored) {}
            }, "root-perf-output");
            drain.setDaemon(true);
            drain.start();

            boolean completed = proc.waitFor(timeoutSeconds, TimeUnit.SECONDS);
            if (!completed) {
                proc.destroyForcibly();
                return new RootResult(false, "timeout");
            }
            drain.join(500);
            return new RootResult(proc.exitValue() == 0, out.toString());
        } catch (Exception e) {
            Log.w(TAG, "root command failed", e);
            if (proc != null) proc.destroyForcibly();
            return new RootResult(false, e.toString());
        }
    }

    private static final class RootResult {
        final boolean ok;
        final String output;
        RootResult(boolean ok, String output) {
            this.ok = ok;
            this.output = output == null ? "" : output;
        }
    }
}
