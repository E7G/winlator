package com.winlator.cmod.core;

public abstract class DefaultVersion {
    public static final String BOX64 = "0.4.2";
    public static final String WOWBOX64 = "0.4.2";
    public static final String FEXCORE = "2601";
    public static final String WRAPPER = "System";

    // Adreno 5xx (including Mi Pad 4 / Adreno 512) cannot use Mesa Turnip.
    // Reusing System here also makes the existing UI choose the direct
    // Freedreno/KGSL OpenGL path without adding device-specific UI branches.
    public static final String WRAPPER_ADRENO = GPUInformation.isAdreno5xxGPU(null)
            ? WRAPPER
            : "turnip26.2.0";

    // DXVK 2.x expects a newer Vulkan feature set than typical A5xx vendor
    // drivers expose. Sarek is kept as the performance-oriented legacy path.
    public static final String DXVK = GPUInformation.isAdreno5xxGPU(null)
            ? "1.11.1-sarek"
            : (GPUInformation.getRenderer(null, null).contains("Mali") ? "1.10.3" : "2.3.1");

    public static final String D8VK = "1.0";
    public static final String VKD3D = "None";
}
