package com.winlator.cmod.xenvironment;

import android.content.Context;

import androidx.appcompat.app.AppCompatActivity;

import com.winlator.cmod.MainActivity;
import com.winlator.cmod.R;
import com.winlator.cmod.SettingsFragment;
import com.winlator.cmod.container.Container;
import com.winlator.cmod.container.ContainerManager;
import com.winlator.cmod.contents.AdrenotoolsManager;
import com.winlator.cmod.core.AppUtils;
import com.winlator.cmod.core.DownloadProgressDialog;
import com.winlator.cmod.core.FileUtils;
import com.winlator.cmod.core.PreloaderDialog;
import com.winlator.cmod.core.TarCompressorUtils;
import com.winlator.cmod.core.WineInfo;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.util.ArrayList;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicLong;

public abstract class ImageFsInstaller {
    public static final byte LATEST_VERSION = 22;
    
    public abstract interface onInstallationFinish {
        public void call();
    }

    private static void resetContainerImgVersions(Context context) {
        ContainerManager manager = new ContainerManager(context);
        for (Container container : manager.getContainers()) {
            container.putExtra("imgVersion", null);
            container.saveData();
        }
    }

    public static boolean installWineArchive(final Context context, String version, File archiveFile) {
        File rootDir = ImageFs.find(context).getRootDir();
        File optDir = new File(rootDir, "opt");
        optDir.mkdirs();

        File outFile = new File(optDir, version);
        File staging = new File(optDir, ".install-" + version);
        FileUtils.delete(outFile);
        FileUtils.delete(staging);
        staging.mkdirs();

        boolean success = TarCompressorUtils.extract(TarCompressorUtils.Type.ZSTD, archiveFile, staging);
        if (!success) success = TarCompressorUtils.extract(TarCompressorUtils.Type.XZ, archiveFile, staging);
        if (!success) {
            FileUtils.delete(staging);
            return false;
        }

        try {
            File profileFile = new File(staging, "profile.json");
            if (profileFile.isFile()) {
                JSONObject profile = new JSONObject(FileUtils.readString(profileFile));
                JSONObject wine = profile.getJSONObject("wine");
                String binPath = wine.getString("binPath");
                String prefixPath = wine.optString("prefixPack", "");

                File binDir = new File(staging, binPath).getCanonicalFile();
                File stagingCanonical = staging.getCanonicalFile();
                if (!binDir.isDirectory() ||
                        !binDir.getPath().startsWith(stagingCanonical.getPath() + File.separator)) {
                    FileUtils.delete(staging);
                    return false;
                }

                File runtimeRoot = binDir.getParentFile();
                if (runtimeRoot == null || !runtimeRoot.isDirectory()) {
                    FileUtils.delete(staging);
                    return false;
                }

                if (!prefixPath.isEmpty()) {
                    File prefixPack = new File(staging, prefixPath).getCanonicalFile();
                    if (prefixPack.isFile() &&
                            prefixPack.getPath().startsWith(stagingCanonical.getPath() + File.separator)) {
                        File normalizedPrefix = new File(runtimeRoot, "prefixPack.txz");
                        if (!prefixPack.equals(normalizedPrefix) && !FileUtils.copy(prefixPack, normalizedPrefix)) {
                            FileUtils.delete(staging);
                            return false;
                        }
                    }
                }

                // Same filesystem: rename preserves Wine symlinks, executable bits and layout.
                if (!runtimeRoot.renameTo(outFile)) {
                    FileUtils.delete(staging);
                    FileUtils.delete(outFile);
                    return false;
                }
                FileUtils.delete(staging);
                return new File(outFile, "bin").isDirectory();
            }

            // Legacy .tar.zst/.txz packages are already rooted at bin/, lib/, ...
            if (!staging.renameTo(outFile)) {
                FileUtils.delete(staging);
                return false;
            }
            return true;
        } catch (Exception e) {
            FileUtils.delete(staging);
            FileUtils.delete(outFile);
            return false;
        }
    }

    public static void installWineFromAssets(final DownloadProgressDialog dialog, final MainActivity activity) {
        String[] versions = activity.getResources().getStringArray(R.array.wine_entries);
        File rootDir = ImageFs.find(activity).getRootDir();
        final byte compressionRatio = 22;

        activity.runOnUiThread(() -> dialog.setMessage(R.string.installing_wine_files));

        for (String version : versions) {
            File outFile = new File(rootDir, "opt/" + version);
            FileUtils.delete(outFile);
            outFile.mkdirs();
            final long contentLength = (long)(FileUtils.getSize(activity, version + ".tar.zst") * (100.0f / compressionRatio));
            AtomicLong totalSizeRef = new AtomicLong();

            TarCompressorUtils.extract(TarCompressorUtils.Type.ZSTD, activity, version + ".tar.zst", outFile, (file, size) -> {
                if (size > 0) {
                    long totalSize = totalSizeRef.addAndGet(size);
                    final int progress = (int)(((float)totalSize / contentLength) * 100);
                    activity.runOnUiThread(() -> dialog.setProgress(progress));
                }
                return file;
            });
         }
    }

    public static void installDriversFromAssets(final DownloadProgressDialog dialog, final MainActivity activity) {
        
        activity.runOnUiThread(() -> dialog.setMessage(R.string.installing_wine_files));
        AdrenotoolsManager adrenotoolsManager = new AdrenotoolsManager(activity);
        String[] adrenotoolsAssetDrivers = activity.getResources().getStringArray(R.array.wrapper_graphics_driver_version_entries);

        for (String driver : adrenotoolsAssetDrivers) {
            final byte compressionRatio = 22;
            final long contentLength = (long)(FileUtils.getSize(activity, adrenotoolsManager.getAssetPath(driver)) * (100.0f / compressionRatio));
            AtomicLong totalSizeRef = new AtomicLong();
            adrenotoolsManager.extractDriverFromResources(driver, (file, size) -> {
                if (size > 0) {
                    long totalSize = totalSizeRef.addAndGet(size);
                    final int progress = (int)(((float)totalSize / contentLength) * 100);
                    activity.runOnUiThread(() -> dialog.setProgress(progress));
                }
                return file;
            });
         }   
    }

    public static void installFromAssets(final MainActivity activity, onInstallationFinish callback) {
        AppUtils.keepScreenOn(activity);
        ImageFs imageFs = ImageFs.find(activity);
        File rootDir = imageFs.getRootDir();

        SettingsFragment.resetEmulatorsVersion(activity);

        final DownloadProgressDialog dialog = new DownloadProgressDialog(activity);
        dialog.show(R.string.installing_system_files);
        
        Executors.newSingleThreadExecutor().execute(() -> {
            clearRootDir(rootDir);
            final byte compressionRatio = 22;
            final long contentLength = (long)(FileUtils.getSize(activity, "imagefs.tar.zst") * (100.0f / compressionRatio));
            AtomicLong totalSizeRef = new AtomicLong();

            boolean success = TarCompressorUtils.extract(TarCompressorUtils.Type.ZSTD, activity, "imagefs.tar.zst", rootDir, (file, size) -> {
                if (size > 0) {
                    long totalSize = totalSizeRef.addAndGet(size);
                    final int progress = (int)(((float)totalSize / contentLength) * 100);
                    activity.runOnUiThread(() -> dialog.setProgress(progress));
                }
                return file;
            });

            if (success) {
                installWineFromAssets(dialog, activity);
                installDriversFromAssets(dialog, activity);
                imageFs.createImgVersionFile(LATEST_VERSION);
                FileUtils.symlink("libSDL2-2.0.so", new File(imageFs.getLibDir(), "libSDL2-2.0.so.0").getAbsolutePath());
                resetContainerImgVersions(activity);
            }
            else AppUtils.showToast(activity, R.string.unable_to_install_system_files);
            
            dialog.closeOnUiThread();
            activity.runOnUiThread(() -> {if (callback != null) callback.call();});
        });
    }

    public static boolean installIfNeeded(final MainActivity activity, onInstallationFinish callback) {
        ImageFs imageFs = ImageFs.find(activity);

        if (!imageFs.isValid() || imageFs.getVersion() < LATEST_VERSION) {
            installFromAssets(activity, callback);
            return true;
        }

        // App upgrades should add the new bundled Proton without wiping an
        // existing imagefs or legacy Proton used by older containers.
        File mainWine = new File(imageFs.getRootDir(), "opt/" + WineInfo.MAIN_WINE_VERSION.identifier());
        File[] mainFiles = mainWine.listFiles();
        if (!mainWine.isDirectory() || mainFiles == null || mainFiles.length == 0) {
            final DownloadProgressDialog dialog = new DownloadProgressDialog(activity);
            dialog.show(R.string.installing_wine_files);
            Executors.newSingleThreadExecutor().execute(() -> {
                installWineFromAssets(dialog, activity);
                dialog.closeOnUiThread();
                activity.runOnUiThread(() -> {
                    if (callback != null) callback.call();
                });
            });
            return true;
        }

        return false;
    }

    private static void clearOptDir(File optDir) {
        File[] files = optDir.listFiles();
        if (files != null) {
            for (File file : files) {
                if (file.getName().equals("installed-wine")) continue;
                FileUtils.delete(file);
            }
        }
    }

    private static void clearRootDir(File rootDir) {
        if (rootDir.isDirectory()) {
            File[] files = rootDir.listFiles();
            if (files != null) {
                for (File file : files) {
                    if (file.isDirectory()) {
                        String name = file.getName();
                        if (name.equals("home")) {
                            continue;
                        }
                    }
                    FileUtils.delete(file);
                }
            }
        }
        else rootDir.mkdirs();
    }
}