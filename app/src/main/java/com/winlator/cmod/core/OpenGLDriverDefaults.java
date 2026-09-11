package com.winlator.cmod.core;

import android.content.Context;

import com.winlator.cmod.container.Container;

import org.json.JSONException;
import org.json.JSONObject;

public final class OpenGLDriverDefaults {
    private static final String INITIALIZED = "openGlDefaultInitialized";
    private static final String ADRENO5XX_PROFILE = "adreno5xxDriverProfileV1";
    private static final String AUTO_MESA_OVERRIDE = "autoMesaGlVersionOverride";
    private static final String MESA_OVERRIDE = "MESA_GL_VERSION_OVERRIDE";
    private static final String A5XX_DXVK = "1.11.1-sarek";

    private OpenGLDriverDefaults() {}

    public static boolean initialize(Context context, JSONObject data) {
        if (data == null) return false;

        try {
            JSONObject extraData = data.optJSONObject("extraData");
            if (extraData == null) extraData = new JSONObject();

            boolean adreno5xx = GPUInformation.isAdreno5xxGPU(context);
            if (adreno5xx) {
                // This marker is intentionally independent from the older generic
                // OpenGL marker so containers created before A5xx support are
                // migrated exactly once.
                if ("1".equals(extraData.optString(ADRENO5XX_PROFILE, "0"))) return false;
            }
            else if ("1".equals(extraData.optString(INITIALIZED, "0"))) {
                return false;
            }

            String config = data.optString("graphicsDriverConfig", Container.DEFAULT_GRAPHICSDRIVERCONFIG);
            String selectedVersion = resolveDriverVersion(context, configValue(config, "version", ';'));
            boolean freedreno = adreno5xx || isTurnipDriver(selectedVersion);
            EnvVars environment = new EnvVars(data.optString("envVars", Container.DEFAULT_ENV_VARS));
            boolean automaticOverride = false;

            if (freedreno && !environment.has(MESA_OVERRIDE)) {
                environment.put(MESA_OVERRIDE, "3.3");
                automaticOverride = true;
            }

            if (adreno5xx) {
                // Adreno 5xx uses the Android/system Vulkan driver and the Mesa
                // Freedreno KGSL OpenGL path. Do not advertise the Vulkan 1.3
                // default intended for modern Turnip drivers.
                config = putConfigValue(config, "vulkanVersion", "1.1", ';');
                config = putConfigValue(config, "version", DefaultVersion.WRAPPER, ';');
                migrateDxvkForAdreno5xx(data);
            }
            else {
                config = putConfigValue(config, "version", selectedVersion, ';');
            }

            data.put("graphicsDriver", freedreno ? "freedreno" : Container.DEFAULT_GRAPHICS_DRIVER);
            data.put("graphicsDriverConfig", config);
            data.put("envVars", environment.toString());
            extraData.put(INITIALIZED, "1");
            extraData.put(AUTO_MESA_OVERRIDE, automaticOverride ? "1" : "0");
            if (adreno5xx) extraData.put(ADRENO5XX_PROFILE, "1");
            data.put("extraData", extraData);
            return true;
        }
        catch (JSONException error) {
            return false;
        }
    }

    public static boolean initialize(Context context, Container container) {
        if (container == null) return false;

        boolean adreno5xx = GPUInformation.isAdreno5xxGPU(context);
        if (adreno5xx) {
            if ("1".equals(container.getExtra(ADRENO5XX_PROFILE, "0"))) return false;
        }
        else if ("1".equals(container.getExtra(INITIALIZED, "0"))) {
            return false;
        }

        String config = container.getGraphicsDriverConfig();
        String selectedVersion = resolveDriverVersion(context, configValue(config, "version", ';'));
        boolean freedreno = adreno5xx || isTurnipDriver(selectedVersion);
        EnvVars environment = new EnvVars(container.getEnvVars());
        boolean automaticOverride = false;

        if (freedreno && !environment.has(MESA_OVERRIDE)) {
            environment.put(MESA_OVERRIDE, "3.3");
            automaticOverride = true;
        }

        if (adreno5xx) {
            config = putConfigValue(config, "vulkanVersion", "1.1", ';');
            config = putConfigValue(config, "version", DefaultVersion.WRAPPER, ';');

            String dxvkConfig = migrateDxvkConfigForAdreno5xx(container.getDXWrapper(), container.getDXWrapperConfig());
            if (dxvkConfig != null) container.setDXWrapperConfig(dxvkConfig);
        }
        else {
            config = putConfigValue(config, "version", selectedVersion, ';');
        }

        container.setGraphicsDriver(freedreno ? "freedreno" : Container.DEFAULT_GRAPHICS_DRIVER);
        container.setGraphicsDriverConfig(config);
        container.setEnvVars(environment.toString());
        container.putExtra(INITIALIZED, "1");
        container.putExtra(AUTO_MESA_OVERRIDE, automaticOverride ? "1" : "0");
        if (adreno5xx) container.putExtra(ADRENO5XX_PROFILE, "1");
        container.saveData();
        return true;
    }

    public static boolean isTurnipDriver(String version) {
        return GPUInformation.isTurnipDriverName(version);
    }

    private static String resolveDriverVersion(Context context, String configuredVersion) {
        if (GPUInformation.isAdreno5xxGPU(context)) return DefaultVersion.WRAPPER;

        String candidate = isTurnipDriver(configuredVersion)
                ? configuredVersion
                : DefaultVersion.WRAPPER_ADRENO;
        try {
            if (GPUInformation.isDriverSupported(candidate, context)) return candidate;
        }
        catch (Throwable ignored) {}
        return DefaultVersion.WRAPPER;
    }

    private static void migrateDxvkForAdreno5xx(JSONObject data) throws JSONException {
        String wrapper = data.optString("dxwrapper", Container.DEFAULT_DXWRAPPER);
        String config = data.optString("dxwrapperConfig", Container.DEFAULT_DXWRAPPERCONFIG);
        String migrated = migrateDxvkConfigForAdreno5xx(wrapper, config);
        if (migrated != null) data.put("dxwrapperConfig", migrated);
    }

    private static String migrateDxvkConfigForAdreno5xx(String wrapper, String config) {
        if (wrapper == null || !wrapper.toLowerCase(java.util.Locale.ROOT).contains("dxvk")) return null;

        String source = (config == null || config.isEmpty()) ? Container.DEFAULT_DXWRAPPERCONFIG : config;
        String currentVersion = configValue(source, "version", ',');

        // Only replace the former project defaults. Preserve an explicit custom
        // legacy DXVK selection made by the user.
        if (currentVersion.isEmpty()
                || "2.3.1".equalsIgnoreCase(currentVersion)
                || "2.3.1-arm64ec-gplasync".equalsIgnoreCase(currentVersion)) {
            return putConfigValue(source, "version", A5XX_DXVK, ',');
        }
        return source;
    }

    private static String configValue(String config, String key, char delimiter) {
        if (config == null || config.isEmpty()) return "";
        String prefix = key + "=";
        for (String item : config.split(java.util.regex.Pattern.quote(String.valueOf(delimiter)), -1)) {
            if (item.startsWith(prefix)) return item.substring(prefix.length());
        }
        return "";
    }

    private static String putConfigValue(String config, String key, String value, char delimiter) {
        String source = config == null ? "" : config;
        String prefix = key + "=";
        String[] items = source.split(java.util.regex.Pattern.quote(String.valueOf(delimiter)), -1);
        StringBuilder result = new StringBuilder();
        boolean replaced = false;

        for (String item : items) {
            if (result.length() > 0) result.append(delimiter);
            if (item.startsWith(prefix)) {
                result.append(prefix).append(value);
                replaced = true;
            }
            else {
                result.append(item);
            }
        }

        if (!replaced) {
            if (result.length() > 0 && result.charAt(result.length() - 1) != delimiter) result.append(delimiter);
            result.append(prefix).append(value);
        }
        return result.toString();
    }
}
