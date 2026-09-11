package com.winlator.cmod.core;

import android.content.Context;

import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public abstract class GPUInformation {
    private static final Pattern ADRENO_MODEL_PATTERN = Pattern.compile(
            "(?i)adreno(?:\\s*\\(tm\\))?\\s*(\\d{3})"
    );

    private static String getSystemRenderer(Context context) {
        try {
            String renderer = getRenderer(null, context);
            return renderer != null ? renderer : "";
        }
        catch (Throwable ignored) {
            return "";
        }
    }

    public static boolean isAdrenoGPU(Context context) {
        return getSystemRenderer(context).toLowerCase(Locale.ROOT).contains("adreno");
    }

    /**
     * Returns the numeric Adreno model reported by the Android Vulkan/OpenGL stack
     * (for example 512 or 650), or -1 when it cannot be identified.
     */
    public static int getAdrenoModel(Context context) {
        Matcher matcher = ADRENO_MODEL_PATTERN.matcher(getSystemRenderer(context));
        if (!matcher.find()) return -1;

        try {
            return Integer.parseInt(matcher.group(1));
        }
        catch (NumberFormatException ignored) {
            return -1;
        }
    }

    public static boolean isAdreno5xxGPU(Context context) {
        int model = getAdrenoModel(context);
        return model >= 500 && model < 600;
    }

    public static boolean isTurnipDriverName(String driverName) {
        String value = driverName == null ? "" : driverName.toLowerCase(Locale.ROOT);
        return value.contains("turnip") || value.startsWith("tu-") || value.startsWith("mesa-turnip");
    }

    public static boolean isDriverSupported(String driverName, Context context) {
        if (driverName == null || driverName.isEmpty()) return false;

        // Mesa Turnip targets Adreno 6xx and newer. Probing it on A5xx may appear
        // successful on some vendor stacks but it is not a usable A5xx Vulkan path.
        if (isAdreno5xxGPU(context) && isTurnipDriverName(driverName)) return false;

        if (!isAdrenoGPU(context) && !driverName.equalsIgnoreCase("System")) return false;

        try {
            String renderer = getRenderer(driverName, context);
            return renderer != null && !renderer.toLowerCase(Locale.ROOT).contains("unknown");
        }
        catch (Throwable ignored) {
            return false;
        }
    }

    public native static String getVulkanVersion(String driverName, Context context);
    public native static int getVendorID(String driverName, Context context);
    public native static String getRenderer(String driverName, Context context);
    public native static String[] enumerateExtensions(String driverName, Context context);

    static {
        System.loadLibrary("winlator");
    }
}
