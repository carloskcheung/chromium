// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.net;

import android.Manifest;
import android.annotation.TargetApi;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.TrafficStats;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Build.VERSION_CODES;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.security.NetworkSecurityPolicy;
import android.telephony.TelephonyManager;
import android.util.Log;

import org.chromium.base.ContextUtils;
import org.chromium.base.VisibleForTesting;
import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.annotations.CalledByNativeUnchecked;
import org.chromium.base.annotations.MainDex;
import org.chromium.base.compat.ApiHelperForM;
import org.chromium.base.metrics.RecordHistogram;

import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.net.SocketAddress;
import java.net.SocketException;
import java.net.SocketImpl;
import java.net.URLConnection;
import java.net.UnknownHostException;
import java.security.KeyStoreException;
import java.security.NoSuchAlgorithmException;
import java.security.cert.CertificateException;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * This class implements net utilities required by the net component.
 */
@MainDex
class AndroidNetworkLibrary {
    private static final String TAG = "AndroidNetworkLibrary";

    // Cached Method for LinkProperties.isPrivateDnsActive().
    private static Method sIsPrivateDnsActiveMethod;
    // Cached Method for LinkProperties.getPrivateDnsServerName().
    private static Method sGetPrivateDnsServerNameMethod;
    // Cached value indicating if app has ACCESS_NETWORK_STATE permission.
    private static Boolean sHaveAccessNetworkState;

    // Set of public DNS servers supporting DNS-over-HTTPS.
    private static final Set<InetAddress> sAutoDohServers = new HashSet<>();
    // Set of public DNS-over-TLS servers supporting DNS-over-HTTPS.
    private static final Set<String> sAutoDohDotServers = new HashSet<>();

    static {
        try {
            // Populate set of public DNS servers supporting DNS-over-HTTPS.

            // Google Public DNS
            sAutoDohServers.add(InetAddress.getByName("8.8.8.8"));
            sAutoDohServers.add(InetAddress.getByName("8.8.4.4"));
            sAutoDohServers.add(InetAddress.getByName("2001:4860:4860::8888"));
            sAutoDohServers.add(InetAddress.getByName("2001:4860:4860::8844"));
            // Cloudflare DNS
            sAutoDohServers.add(InetAddress.getByName("1.1.1.1"));
            sAutoDohServers.add(InetAddress.getByName("1.0.0.1"));
            sAutoDohServers.add(InetAddress.getByName("2606:4700:4700::1111"));
            sAutoDohServers.add(InetAddress.getByName("2606:4700:4700::1001"));
            // Quad9 DNS
            sAutoDohServers.add(InetAddress.getByName("9.9.9.9"));
            sAutoDohServers.add(InetAddress.getByName("149.112.112.112"));
            sAutoDohServers.add(InetAddress.getByName("2620:fe::fe"));
            sAutoDohServers.add(InetAddress.getByName("2620:fe::9"));
        } catch (UnknownHostException e) {
            throw new RuntimeException("Failed to parse IP addresses", e);
        }

        // Populate set of public DNS-over-TLS servers supporting DNS-over-HTTPS.

        // Google Public DNS
        sAutoDohDotServers.add("dns.google");
        // Cloudflare DNS
        sAutoDohDotServers.add("1dot1dot1dot1.cloudflare-dns.com");
        sAutoDohDotServers.add("cloudflare-dns.com");
        // Quad9 DNS
        sAutoDohDotServers.add("dns.quad9.net");
    }

    /**
     * @return the mime type (if any) that is associated with the file
     *         extension. Returns null if no corresponding mime type exists.
     */
    @CalledByNative
    public static String getMimeTypeFromExtension(String extension) {
        return URLConnection.guessContentTypeFromName("foo." + extension);
    }

    /**
     * @return true if it can determine that only loopback addresses are
     *         configured. i.e. if only 127.0.0.1 and ::1 are routable. Also
     *         returns false if it cannot determine this.
     */
    @CalledByNative
    public static boolean haveOnlyLoopbackAddresses() {
        Enumeration<NetworkInterface> list = null;
        try {
            list = NetworkInterface.getNetworkInterfaces();
            if (list == null) return false;
        } catch (Exception e) {
            Log.w(TAG, "could not get network interfaces: " + e);
            return false;
        }

        while (list.hasMoreElements()) {
            NetworkInterface netIf = list.nextElement();
            try {
                if (netIf.isUp() && !netIf.isLoopback()) return false;
            } catch (SocketException e) {
                continue;
            }
        }
        return true;
    }

    /**
     * Validate the server's certificate chain is trusted. Note that the caller
     * must still verify the name matches that of the leaf certificate.
     *
     * @param certChain The ASN.1 DER encoded bytes for certificates.
     * @param authType The key exchange algorithm name (e.g. RSA).
     * @param host The hostname of the server.
     * @return Android certificate verification result code.
     */
    @CalledByNative
    public static AndroidCertVerifyResult verifyServerCertificates(byte[][] certChain,
                                                                   String authType,
                                                                   String host) {
        try {
            return X509Util.verifyServerCertificates(certChain, authType, host);
        } catch (KeyStoreException e) {
            return new AndroidCertVerifyResult(CertVerifyStatusAndroid.FAILED);
        } catch (NoSuchAlgorithmException e) {
            return new AndroidCertVerifyResult(CertVerifyStatusAndroid.FAILED);
        } catch (IllegalArgumentException e) {
            return new AndroidCertVerifyResult(CertVerifyStatusAndroid.FAILED);
        }
    }

    /**
     * Adds a test root certificate to the local trust store.
     * @param rootCert DER encoded bytes of the certificate.
     */
    @CalledByNativeUnchecked
    public static void addTestRootCertificate(byte[] rootCert) throws CertificateException,
            KeyStoreException, NoSuchAlgorithmException {
        X509Util.addTestRootCertificate(rootCert);
    }

    /**
     * Removes all test root certificates added by |addTestRootCertificate| calls from the local
     * trust store.
     */
    @CalledByNativeUnchecked
    public static void clearTestRootCertificates() throws NoSuchAlgorithmException,
            CertificateException, KeyStoreException {
        X509Util.clearTestRootCertificates();
    }

    /**
     * Returns the ISO country code equivalent of the current MCC.
     */
    @CalledByNative
    private static String getNetworkCountryIso() {
        TelephonyManager telephonyManager =
                (TelephonyManager) ContextUtils.getApplicationContext().getSystemService(
                        Context.TELEPHONY_SERVICE);
        if (telephonyManager == null) return "";
        return telephonyManager.getNetworkCountryIso();
    }

    /**
     * Returns the MCC+MNC (mobile country code + mobile network code) as
     * the numeric name of the current registered operator.
     */
    @CalledByNative
    private static String getNetworkOperator() {
        TelephonyManager telephonyManager =
                (TelephonyManager) ContextUtils.getApplicationContext().getSystemService(
                        Context.TELEPHONY_SERVICE);
        if (telephonyManager == null) return "";
        return telephonyManager.getNetworkOperator();
    }

    /**
     * Returns the MCC+MNC (mobile country code + mobile network code) as
     * the numeric name of the current SIM operator.
     */
    @CalledByNative
    private static String getSimOperator() {
        TelephonyManager telephonyManager =
                (TelephonyManager) ContextUtils.getApplicationContext().getSystemService(
                        Context.TELEPHONY_SERVICE);
        if (telephonyManager == null) return "";
        return telephonyManager.getSimOperator();
    }

    /**
     * Indicates whether the device is roaming on the currently active network. When true, it
     * suggests that use of data may incur extra costs.
     */
    @CalledByNative
    private static boolean getIsRoaming() {
        ConnectivityManager connectivityManager =
                (ConnectivityManager) ContextUtils.getApplicationContext().getSystemService(
                        Context.CONNECTIVITY_SERVICE);
        NetworkInfo networkInfo = connectivityManager.getActiveNetworkInfo();
        if (networkInfo == null) return false; // No active network.
        return networkInfo.isRoaming();
    }

    /**
     * Returns true if the system's captive portal probe was blocked for the current default data
     * network. The method will return false if the captive portal probe was not blocked, the login
     * process to the captive portal has been successfully completed, or if the captive portal
     * status can't be determined. Requires ACCESS_NETWORK_STATE permission. Only available on
     * Android Marshmallow and later versions. Returns false on earlier versions.
     */
    @TargetApi(Build.VERSION_CODES.M)
    @CalledByNative
    private static boolean getIsCaptivePortal() {
        // NetworkCapabilities.NET_CAPABILITY_CAPTIVE_PORTAL is only available on Marshmallow and
        // later versions.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return false;
        ConnectivityManager connectivityManager =
                (ConnectivityManager) ContextUtils.getApplicationContext().getSystemService(
                        Context.CONNECTIVITY_SERVICE);
        if (connectivityManager == null) return false;

        Network network = ApiHelperForM.getActiveNetwork(connectivityManager);
        if (network == null) return false;

        NetworkCapabilities capabilities = connectivityManager.getNetworkCapabilities(network);
        return capabilities != null
                && capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_CAPTIVE_PORTAL);
    }

    /**
     * Gets the SSID of the currently associated WiFi access point if there is one, and it is
     * available. SSID may not be available if the app does not have permissions to access it. On
     * Android M+, the app accessing SSID needs to have ACCESS_COARSE_LOCATION or
     * ACCESS_FINE_LOCATION. If there is no WiFi access point or its SSID is unavailable, an empty
     * string is returned.
     */
    @CalledByNative
    public static String getWifiSSID() {
        final Intent intent = ContextUtils.getApplicationContext().registerReceiver(
                null, new IntentFilter(WifiManager.NETWORK_STATE_CHANGED_ACTION));
        if (intent != null) {
            final WifiInfo wifiInfo = intent.getParcelableExtra(WifiManager.EXTRA_WIFI_INFO);
            if (wifiInfo != null) {
                final String ssid = wifiInfo.getSSID();
                // On Android M+, the platform APIs may return "<unknown ssid>" as the SSID if the
                // app does not have sufficient permissions. In that case, return an empty string.
                if (ssid != null && !ssid.equals("<unknown ssid>")) {
                    return ssid;
                }
            }
        }
        return "";
    }

    public static class NetworkSecurityPolicyProxy {
        private static NetworkSecurityPolicyProxy sInstance = new NetworkSecurityPolicyProxy();

        public static NetworkSecurityPolicyProxy getInstance() {
            return sInstance;
        }

        @VisibleForTesting
        public static void setInstanceForTesting(
                NetworkSecurityPolicyProxy networkSecurityPolicyProxy) {
            sInstance = networkSecurityPolicyProxy;
        }

        @TargetApi(Build.VERSION_CODES.N)
        public boolean isCleartextTrafficPermitted(String host) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) {
                // No per-host configuration before N.
                return isCleartextTrafficPermitted();
            }
            return NetworkSecurityPolicy.getInstance().isCleartextTrafficPermitted(host);
        }

        @TargetApi(Build.VERSION_CODES.M)
        public boolean isCleartextTrafficPermitted() {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
                // Always true before M.
                return true;
            }
            return NetworkSecurityPolicy.getInstance().isCleartextTrafficPermitted();
        }
    }

    /**
     * Returns true if cleartext traffic to |host| is allowed by the current app.
     */
    @CalledByNative
    private static boolean isCleartextPermitted(String host) {
        try {
            return NetworkSecurityPolicyProxy.getInstance().isCleartextTrafficPermitted(host);
        } catch (IllegalArgumentException e) {
            return NetworkSecurityPolicyProxy.getInstance().isCleartextTrafficPermitted();
        }
    }

    /**
     * @returns result of linkProperties.isPrivateDnsActive().
     */
    static boolean isPrivateDnsActive(LinkProperties linkProperties) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && linkProperties != null) {
            // TODO(pauljensen): When Android P SDK is available, remove reflection.
            try {
                // This could be racy if called on multiple threads, but races will
                // end in the same result so it's not a problem.
                if (sIsPrivateDnsActiveMethod == null) {
                    sIsPrivateDnsActiveMethod =
                            linkProperties.getClass().getMethod("isPrivateDnsActive");
                }
                return ((Boolean) sIsPrivateDnsActiveMethod.invoke(linkProperties)).booleanValue();
            } catch (Exception e) {
                Log.e(TAG, "Can not call LinkProperties.isPrivateDnsActive():", e);
            }
        }
        return false;
    }

    /**
     * @returns result of linkProperties.getPrivateDnsServerName().
     */
    private static String getPrivateDnsServerName(LinkProperties linkProperties) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && linkProperties != null) {
            // TODO(pauljensen): When Android P SDK is available, remove reflection.
            try {
                // This could be racy if called on multiple threads, but races will
                // end in the same result so it's not a problem.
                if (sGetPrivateDnsServerNameMethod == null) {
                    sGetPrivateDnsServerNameMethod =
                            linkProperties.getClass().getMethod("getPrivateDnsServerName");
                }
                return (String) sGetPrivateDnsServerNameMethod.invoke(linkProperties);
            } catch (Exception e) {
                Log.e(TAG, "Can not call LinkProperties.getPrivateDnsServerName():", e);
            }
        }
        return null;
    }

    private static boolean haveAccessNetworkState() {
        // This could be racy if called on multiple threads, but races will
        // end in the same result so it's not a problem.
        if (sHaveAccessNetworkState == null) {
            sHaveAccessNetworkState =
                    Boolean.valueOf(ContextUtils.getApplicationContext().checkPermission(
                                            Manifest.permission.ACCESS_NETWORK_STATE,
                                            Process.myPid(), Process.myUid())
                            == PackageManager.PERMISSION_GRANTED);
        }
        return sHaveAccessNetworkState;
    }

    /**
     * Returns list of IP addresses of DNS servers.
     * If private DNS is active, then returns a 1x1 array.
     */
    @TargetApi(Build.VERSION_CODES.M)
    @CalledByNative
    private static byte[][] getDnsServers() {
        if (!haveAccessNetworkState()) {
            return new byte[0][0];
        }
        ConnectivityManager connectivityManager =
                (ConnectivityManager) ContextUtils.getApplicationContext().getSystemService(
                        Context.CONNECTIVITY_SERVICE);
        if (connectivityManager == null) {
            return new byte[0][0];
        }
        Network network = ApiHelperForM.getActiveNetwork(connectivityManager);
        if (network == null) {
            return new byte[0][0];
        }
        LinkProperties linkProperties = connectivityManager.getLinkProperties(network);
        if (linkProperties == null) {
            return new byte[0][0];
        }
        List<InetAddress> dnsServersList = linkProperties.getDnsServers();
        // Determine if any DNS servers could be auto-upgraded to DNS-over-HTTPS.
        boolean autoDoh = false;
        for (InetAddress dnsServer : dnsServersList) {
            if (sAutoDohServers.contains(dnsServer)) {
                autoDoh = true;
                break;
            }
        }
        if (isPrivateDnsActive(linkProperties)) {
            String privateDnsServerName = getPrivateDnsServerName(linkProperties);
            // If user explicitly selected a DNS-over-TLS server...
            if (privateDnsServerName != null) {
                // ...their DNS-over-HTTPS support depends on the DNS-over-TLS server name.
                autoDoh = sAutoDohDotServers.contains(privateDnsServerName.toLowerCase(Locale.US));
            }
            RecordHistogram.recordBooleanHistogram(
                    "Net.DNS.Android.DotExplicit", privateDnsServerName != null);
            RecordHistogram.recordBooleanHistogram("Net.DNS.Android.AutoDohPrivate", autoDoh);
            return new byte[1][1];
        }
        RecordHistogram.recordBooleanHistogram("Net.DNS.Android.AutoDohPublic", autoDoh);
        byte[][] dnsServers = new byte[dnsServersList.size()][];
        for (int i = 0; i < dnsServersList.size(); i++) {
            dnsServers[i] = dnsServersList.get(i).getAddress();
        }
        return dnsServers;
    }

    /**
     * Class to wrap FileDescriptor.setInt$() which is hidden and so must be accessed via
     * reflection.
     */
    private static class SetFileDescriptor {
        // Reference to FileDescriptor.setInt$(int fd).
        private static final Method sFileDescriptorSetInt;

        // Get reference to FileDescriptor.setInt$(int fd) via reflection.
        static {
            try {
                sFileDescriptorSetInt = FileDescriptor.class.getMethod("setInt$", Integer.TYPE);
            } catch (NoSuchMethodException | SecurityException e) {
                throw new RuntimeException("Unable to get FileDescriptor.setInt$", e);
            }
        }

        /** Creates a FileDescriptor and calls FileDescriptor.setInt$(int fd) on it. */
        public static FileDescriptor createWithFd(int fd) {
            try {
                FileDescriptor fileDescriptor = new FileDescriptor();
                sFileDescriptorSetInt.invoke(fileDescriptor, fd);
                return fileDescriptor;
            } catch (IllegalAccessException e) {
                throw new RuntimeException("FileDescriptor.setInt$() failed", e);
            } catch (InvocationTargetException e) {
                throw new RuntimeException("FileDescriptor.setInt$() failed", e);
            }
        }
    }

    /**
     * This class provides an implementation of {@link java.net.Socket} that serves only as a
     * conduit to pass a file descriptor integer to {@link android.net.TrafficStats#tagSocket}
     * when called by {@link #tagSocket}. This class does not take ownership of the file descriptor,
     * so calling {@link #close} will not actually close the file descriptor.
     */
    private static class SocketFd extends Socket {
        /**
         * This class provides an implementation of {@link java.net.SocketImpl} that serves only as
         * a conduit to pass a file descriptor integer to {@link android.net.TrafficStats#tagSocket}
         * when called by {@link #tagSocket}. This class does not take ownership of the file
         * descriptor, so calling {@link #close} will not actually close the file descriptor.
         */
        private static class SocketImplFd extends SocketImpl {
            /**
             * Create a {@link java.net.SocketImpl} that sets {@code fd} as the underlying file
             * descriptor. Does not take ownership of the file descriptor, so calling {@link #close}
             * will not actually close the file descriptor.
             */
            SocketImplFd(FileDescriptor fd) {
                this.fd = fd;
            }

            @Override
            protected void accept(SocketImpl s) {
                throw new RuntimeException("accept not implemented");
            }
            @Override
            protected int available() {
                throw new RuntimeException("accept not implemented");
            }
            @Override
            protected void bind(InetAddress host, int port) {
                throw new RuntimeException("accept not implemented");
            }
            @Override
            protected void close() {}
            @Override
            protected void connect(InetAddress address, int port) {
                throw new RuntimeException("connect not implemented");
            }
            @Override
            protected void connect(SocketAddress address, int timeout) {
                throw new RuntimeException("connect not implemented");
            }
            @Override
            protected void connect(String host, int port) {
                throw new RuntimeException("connect not implemented");
            }
            @Override
            protected void create(boolean stream) {}
            @Override
            protected InputStream getInputStream() {
                throw new RuntimeException("getInputStream not implemented");
            }
            @Override
            protected OutputStream getOutputStream() {
                throw new RuntimeException("getOutputStream not implemented");
            }
            @Override
            protected void listen(int backlog) {
                throw new RuntimeException("listen not implemented");
            }
            @Override
            protected void sendUrgentData(int data) {
                throw new RuntimeException("sendUrgentData not implemented");
            }
            @Override
            public Object getOption(int optID) {
                throw new RuntimeException("getOption not implemented");
            }
            @Override
            public void setOption(int optID, Object value) {
                throw new RuntimeException("setOption not implemented");
            }
        }

        /**
         * Create a {@link java.net.Socket} that sets {@code fd} as the underlying file
         * descriptor. Does not take ownership of the file descriptor, so calling {@link #close}
         * will not actually close the file descriptor.
         */
        SocketFd(FileDescriptor fd) throws IOException {
            super(new SocketImplFd(fd));
        }
    }

    /**
     * Tag socket referenced by {@code ifd} with {@code tag} for UID {@code uid}.
     *
     * Assumes thread UID tag isn't set upon entry, and ensures thread UID tag isn't set upon exit.
     * Unfortunately there is no TrafficStatis.getThreadStatsUid().
     */
    @CalledByNative
    private static void tagSocket(int ifd, int uid, int tag) throws IOException {
        // Set thread tags.
        int oldTag = TrafficStats.getThreadStatsTag();
        if (tag != oldTag) {
            TrafficStats.setThreadStatsTag(tag);
        }
        if (uid != TrafficStatsUid.UNSET) {
            ThreadStatsUid.set(uid);
        }

        // Apply thread tags to socket.

        // First, convert integer file descriptor (ifd) to FileDescriptor.
        final ParcelFileDescriptor pfd;
        final FileDescriptor fd;
        // The only supported way to generate a FileDescriptor from an integer file
        // descriptor is via ParcelFileDescriptor.adoptFd(). Unfortunately prior to Android
        // Marshmallow ParcelFileDescriptor.detachFd() didn't actually detach from the
        // FileDescriptor, so use reflection to set {@code fd} into the FileDescriptor for
        // versions prior to Marshmallow. Here's the fix that went into Marshmallow:
        // https://android.googlesource.com/platform/frameworks/base/+/b30ad6f
        if (Build.VERSION.SDK_INT < VERSION_CODES.M) {
            pfd = null;
            fd = SetFileDescriptor.createWithFd(ifd);
        } else {
            pfd = ParcelFileDescriptor.adoptFd(ifd);
            fd = pfd.getFileDescriptor();
        }
        // Second, convert FileDescriptor to Socket.
        Socket s = new SocketFd(fd);
        // Third, tag the Socket.
        TrafficStats.tagSocket(s);
        s.close(); // No-op but always good to close() Closeables.
        // Have ParcelFileDescriptor relinquish ownership of the file descriptor.
        if (pfd != null) {
            pfd.detachFd();
        }

        // Restore prior thread tags.
        if (tag != oldTag) {
            TrafficStats.setThreadStatsTag(oldTag);
        }
        if (uid != TrafficStatsUid.UNSET) {
            ThreadStatsUid.clear();
        }
    }
}
