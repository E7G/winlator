package com.winlator.cmod.contents;

import com.winlator.cmod.core.Callback;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLConnection;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public class Downloader {
    private static final int CONNECT_TIMEOUT_MS = 10_000;
    private static final int READ_TIMEOUT_MS = 45_000;
    private static final String USER_AGENT = "Winlator-E7G/4.0";

    // Mainland-friendly GitHub proxies. The official URL is always kept as the final fallback.
    private static final String PRIMARY_GITHUB_PROXY = "https://gh-proxy.com/";
    private static final String SECONDARY_GITHUB_PROXY = "https://ghproxy.net/";

    public static boolean downloadFile(String address, File file) {
        return downloadFile(address, file, null);
    }

    public static boolean downloadFile(String address, File file, Callback<Integer> progressCallback) {
        if (address == null || address.trim().isEmpty() || file == null) return false;

        File parent = file.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) return false;
        File partial = new File(file.getAbsolutePath() + ".part");

        for (String candidate : getDownloadCandidates(address)) {
            deleteQuietly(partial);
            URLConnection connection = null;
            try {
                if (progressCallback != null) progressCallback.call(0);
                connection = openConnection(candidate);
                long contentLength = connection.getContentLengthLong();
                String contentType = connection.getContentType();
                if (contentType != null && contentType.toLowerCase(Locale.ENGLISH).contains("text/html")) {
                    throw new IllegalStateException("Unexpected HTML response from " + candidate);
                }

                long downloadedSize = 0;
                int lastProgress = -1;
                try (InputStream input = connection.getInputStream();
                     OutputStream output = new FileOutputStream(partial)) {
                    byte[] data = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(data)) != -1) {
                        output.write(data, 0, count);
                        downloadedSize += count;
                        if (progressCallback != null && contentLength > 0) {
                            int progress = Math.min(99, (int) (downloadedSize * 100 / contentLength));
                            if (progress != lastProgress) {
                                lastProgress = progress;
                                progressCallback.call(progress);
                            }
                        }
                    }
                    output.flush();
                }

                if (downloadedSize <= 0) throw new IllegalStateException("Empty response from " + candidate);
                if (contentLength > 0 && downloadedSize != contentLength) {
                    throw new IllegalStateException("Incomplete download from " + candidate
                            + ": expected " + contentLength + ", got " + downloadedSize);
                }

                if (!replaceFile(partial, file)) {
                    throw new IllegalStateException("Unable to move downloaded file into place");
                }
                if (progressCallback != null) progressCallback.call(100);
                disconnect(connection);
                return true;
            } catch (Exception e) {
                e.printStackTrace();
                deleteQuietly(partial);
                disconnect(connection);
            }
        }

        deleteQuietly(partial);
        return false;
    }

    public static String downloadString(String address) {
        if (address == null || address.trim().isEmpty()) return null;

        for (String candidate : getDownloadCandidates(address)) {
            URLConnection connection = null;
            try {
                connection = openConnection(candidate);
                StringBuilder sb = new StringBuilder();
                try (InputStream input = connection.getInputStream();
                     BufferedReader reader = new BufferedReader(new InputStreamReader(input, StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = reader.readLine()) != null) sb.append(line).append('\n');
                }

                String result = sb.toString();
                String trimmed = result.trim().toLowerCase(Locale.ENGLISH);
                if (trimmed.isEmpty()) throw new IllegalStateException("Empty response from " + candidate);
                if (trimmed.startsWith("<!doctype html") || trimmed.startsWith("<html")) {
                    throw new IllegalStateException("Unexpected HTML response from " + candidate);
                }
                disconnect(connection);
                return result;
            } catch (Exception e) {
                e.printStackTrace();
                disconnect(connection);
            }
        }
        return null;
    }

    /**
     * Builds candidates in priority order: mainland-friendly primary mirror, backup mirror,
     * then the original GitHub URL. Non-GitHub URLs are returned unchanged.
     */
    public static List<String> getDownloadCandidates(String address) {
        Set<String> candidates = new LinkedHashSet<>();
        try {
            URL url = new URL(address);
            String host = url.getHost().toLowerCase(Locale.ENGLISH);

            // Avoid nesting a proxy URL inside another proxy URL.
            if (host.equals("gh-proxy.com") || host.equals("ghproxy.net")) {
                candidates.add(address);
                return new ArrayList<>(candidates);
            }

            boolean githubFileHost = host.equals("github.com")
                    || host.equals("raw.githubusercontent.com")
                    || host.equals("gist.githubusercontent.com")
                    || host.equals("objects.githubusercontent.com")
                    || host.endsWith(".githubusercontent.com");

            if (githubFileHost) {
                candidates.add(PRIMARY_GITHUB_PROXY + address);
                candidates.add(SECONDARY_GITHUB_PROXY + address);
            } else if (host.equals("api.github.com")) {
                // gh-proxy.com supports GitHub API requests; ghproxy.net is kept for file traffic only.
                candidates.add(PRIMARY_GITHUB_PROXY + address);
            }
        } catch (Exception ignored) {
        }

        candidates.add(address);
        return new ArrayList<>(candidates);
    }

    private static URLConnection openConnection(String address) throws Exception {
        URLConnection connection = new URL(address).openConnection();
        connection.setConnectTimeout(CONNECT_TIMEOUT_MS);
        connection.setReadTimeout(READ_TIMEOUT_MS);
        connection.setRequestProperty("User-Agent", USER_AGENT);
        connection.setRequestProperty("Accept", "*/*");
        if (connection instanceof HttpURLConnection) {
            HttpURLConnection http = (HttpURLConnection) connection;
            http.setInstanceFollowRedirects(true);
            int code = http.getResponseCode();
            if (code < 200 || code >= 300) {
                http.disconnect();
                throw new IllegalStateException("HTTP " + code + " from " + address);
            }
        } else {
            connection.connect();
        }
        return connection;
    }

    private static boolean replaceFile(File source, File target) {
        if (target.exists() && !target.delete()) return false;
        if (source.renameTo(target)) return true;

        try (InputStream input = new FileInputStream(source);
             OutputStream output = new FileOutputStream(target)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = input.read(buffer)) != -1) output.write(buffer, 0, read);
            output.flush();
            deleteQuietly(source);
            return true;
        } catch (Exception e) {
            deleteQuietly(target);
            return false;
        }
    }

    private static void disconnect(URLConnection connection) {
        if (connection instanceof HttpURLConnection) {
            ((HttpURLConnection) connection).disconnect();
        }
    }

    private static void deleteQuietly(File file) {
        if (file != null && file.exists()) file.delete();
    }
}
