import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;

import javax.xml.parsers.DocumentBuilderFactory;

import org.apache.felix.framework.Felix;
import org.apache.felix.framework.util.Util;
import org.osgi.framework.Bundle;
import org.osgi.framework.BundleContext;
import org.osgi.framework.Constants;
import org.w3c.dom.Document;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

/**
 * Spec for the published Weasis launcher in Applications/Php/Weasis.
 *
 * loadWeasisX.php sends the browser to weasisX.jnlp. Java Web Start starts
 * org.weasis.launcher.WebstartLauncher, which copies each -VMP argument into a
 * system property and reads felix.config.properties with URL.openConnection.
 * felix.auto.start locations are passed to BundleContext.installBundle(location,
 * null). Felix 4.2.1 (the framework Weasis 1.2.8 declares) opens that URL and
 * copies the body with no checksum, then AutoProcessor calls Bundle.start().
 *
 * These tests fail while a cleartext substitute of that download is started.
 * They pass once every code URL in the launcher is https, so a cleartext
 * interceptor cannot replace the jar.
 *
 * Left untested because they are not this code path: the commented-out
 * substance.jnlp extension, the j2se autodownload href, icon hrefs, the
 * $dicom:get weasisX.xml image list, the Orthanc base URL in loadWeasisX.php,
 * and the gosh telnet console argument. Java Web Start rejects an unsigned
 * substitute of weasis-launcher.jar or felix.jar before main runs; that gate
 * is not reimplemented here. The bundle those jars then download is what runs.
 * weasis.i18n is only checked as a cleartext URL: loadTranslationBundle installs
 * that jar and does not call Bundle.start() itself.
 */
public class WeasisHttpCodeExecutionTest {
    private static final String JNLP = "Applications/Php/Weasis/weasisX.jnlp";
    private static final String PHP = "Applications/Php/Weasis/loadWeasisX.php";

    private static int failures = 0;

    public static void main(String[] args) throws Exception {
        if (args.length != 2) {
            throw new IllegalArgumentException("usage: WeasisHttpCodeExecutionTest <repo> <marker.jar>");
        }
        File repo = new File(args[0]);
        File markerJar = new File(args[1]);
        Document jnlp = parse(new File(repo, JNLP));
        Map<String, String> arguments = vmpArguments(jnlp);

        expectHttps(redirectTarget(new File(repo, PHP)), "loadWeasisX.php window.location");
        expectHttps(attribute(jnlp.getDocumentElement(), "codebase"), "jnlp codebase");
        for (Element jar : elements(jnlp, "jar")) {
            expectHttps(attribute(jar, "href"), "jar href");
        }
        for (String key : new String[] {
            "felix.config.properties",
            "felix.extended.config.properties",
            "weasis.codebase.url",
            "weasis.i18n"
        }) {
            if (arguments.containsKey(key)) {
                expectHttps(arguments.get(key), key);
            }
        }

        String config = arguments.get("felix.config.properties");
        String extended = arguments.get("felix.extended.config.properties");
        String codebase = arguments.get("weasis.codebase.url");
        if (config != null && codebase != null) {
            expectSubstituteNotStarted("felix.config.properties", config, codebase, markerJar);
        }
        if (extended != null && codebase != null) {
            expectSubstituteNotStarted("felix.extended.config.properties", extended, codebase, markerJar);
        }

        if (failures > 0) {
            System.err.println(failures + " test(s) failed");
            System.exit(1);
        }
        System.out.println("all Weasis launcher tests passed");
    }

    /**
     * The properties file lists ${weasis.codebase.url}/bundle/marker.jar, which is
     * how Weasis conf/config.properties names plug-ins. Cleartext URLs are served
     * from this process in place of the host written in the JNLP. HTTPS URLs are
     * not intercepted. The marker file is created only when the substitute starts.
     */
    private static void expectSubstituteNotStarted(String label, String propertiesUrl, String codebaseUrl,
            File markerJar) throws Exception {
        String testName = "substituted bundle named by " + label + " is not started";
        if (propertiesUrl.startsWith("https://") && codebaseUrl.startsWith("https://")) {
            pass(testName);
            return;
        }
        File marker = File.createTempFile("weasis-marker-", ".txt");
        marker.delete();
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        int port = server.getAddress().getPort();
        byte[] bundle = readAll(markerJar);
        // Always serve from this process. Never open the host named in the JNLP.
        String localProperties = "http://127.0.0.1:" + port + pathOf(propertiesUrl);
        boolean codebaseCleartext = codebaseUrl.startsWith("http://");
        String localCodebase = "http://127.0.0.1:" + port + pathOf(codebaseUrl);
        String propertiesBody = codebaseCleartext
                ? "felix.auto.start.1=${weasis.codebase.url}/bundle/marker.jar\n"
                : "felix.auto.start.1=http://127.0.0.1:" + port + "/bundle/marker.jar\n";
        server.createContext("/", exchange -> {
            String path = exchange.getRequestURI().getPath();
            if (path.equals(pathOf(propertiesUrl))) {
                send(exchange, 200, "text/plain", propertiesBody.getBytes(StandardCharsets.UTF_8));
            } else if (path.endsWith("/bundle/marker.jar")) {
                send(exchange, 200, "application/java-archive", bundle);
            } else {
                exchange.sendResponseHeaders(404, -1);
                exchange.close();
            }
        });
        server.start();
        File cache = new File(marker.getParentFile(), marker.getName() + "-cache");
        cache.mkdirs();
        String previousMarker = System.setProperty("weasis.test.marker", marker.getAbsolutePath());
        String previousCodebase = System.getProperty("weasis.codebase.url");
        try {
            // WeasisLauncher.launch sets this from the -VMP argument before reading properties.
            System.setProperty("weasis.codebase.url", codebaseCleartext ? localCodebase : codebaseUrl);
            Properties props = new Properties();
            loadProperties(localProperties, props);
            substitute(props);
            props.setProperty(Constants.FRAMEWORK_STORAGE, cache.getAbsolutePath());
            String location = firstAutoStart(props);
            if (location == null) {
                fail(testName, "properties did not name a felix.auto.start bundle");
                return;
            }
            startBundle(props, location);
            if (marker.exists()) {
                fail(testName, "substituted activator wrote " + marker.getAbsolutePath()
                        + " after installBundle of " + location);
            } else {
                pass(testName);
            }
        } finally {
            server.stop(0);
            restore("weasis.test.marker", previousMarker);
            restore("weasis.codebase.url", previousCodebase);
            deleteTree(cache);
            marker.delete();
        }
    }

    private static void startBundle(Properties props, String location) throws Exception {
        // Felix 4.2.1's URL-handler proxy cannot reflect into java.net on this JDK.
        // Leaving the proxy off uses the platform HTTP handler. JarRevision.initialize
        // still opens the bundle location and copies the body with no checksum.
        props.setProperty("felix.service.urlhandlers", "false");
        Felix felix = new Felix(props);
        try {
            felix.init();
            BundleContext context = felix.getBundleContext();
            // AutoProcessor installs and start()s bundles, then WeasisLauncher calls Felix.start().
            Bundle installed = context.installBundle(location, null);
            installed.start();
            felix.start();
        } finally {
            try {
                felix.stop();
                felix.waitForStop(5000);
            } catch (Exception ignored) {
                // The framework may already have stopped after the bundle ran.
            }
        }
    }

    private static void loadProperties(String url, Properties props) throws IOException {
        InputStream in = URI.create(url).toURL().openConnection().getInputStream();
        try {
            props.load(in);
        } finally {
            in.close();
        }
    }

    private static void substitute(Properties props) {
        for (String name : props.stringPropertyNames()) {
            props.setProperty(name, Util.substVars(props.getProperty(name), name, null, props));
        }
    }

    private static String firstAutoStart(Properties props) {
        for (String name : props.stringPropertyNames()) {
            if (name.toLowerCase(Locale.ROOT).startsWith("felix.auto.start")) {
                String value = props.getProperty(name).trim();
                if (!value.isEmpty()) {
                    return value.split("\\s+")[0];
                }
            }
        }
        return null;
    }

    /** -VMP parsing from WeasisLauncher.launch: strip the prefix, split on the first '=', drop quotes. */
    private static Map<String, String> vmpArguments(Document jnlp) {
        Map<String, String> found = new LinkedHashMap<String, String>();
        for (Element argument : elements(jnlp, "argument")) {
            String text = argument.getTextContent().trim();
            if (!text.startsWith("-VMP") || text.length() <= 4) {
                continue;
            }
            String[] vmarg = text.substring(4).split("=", 2);
            if (vmarg.length != 2) {
                continue;
            }
            String value = vmarg[1];
            if (value.startsWith("\"") && value.endsWith("\"") && value.length() >= 2) {
                value = value.substring(1, value.length() - 1);
            }
            found.put(vmarg[0], value);
        }
        return found;
    }

    private static String redirectTarget(File php) throws IOException {
        BufferedReader reader = new BufferedReader(
                new java.io.InputStreamReader(new FileInputStream(php), StandardCharsets.UTF_8));
        try {
            String line;
            while ((line = reader.readLine()) != null) {
                int marker = line.indexOf("window.location");
                if (marker < 0) {
                    continue;
                }
                int quote = line.indexOf('"', marker);
                int end = line.indexOf('"', quote + 1);
                if (quote >= 0 && end > quote) {
                    return line.substring(quote + 1, end);
                }
            }
        } finally {
            reader.close();
        }
        return null;
    }

    private static String pathOf(String url) {
        String path = URI.create(url).getRawPath();
        return path == null ? "" : path;
    }

    private static void expectHttps(String url, String label) {
        String testName = label + " uses https";
        if (url != null && url.startsWith("https://")) {
            pass(testName);
        } else {
            fail(testName, "URL is " + url);
        }
    }

    private static Document parse(File file) throws Exception {
        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(false);
        factory.setExpandEntityReferences(false);
        return factory.newDocumentBuilder().parse(file);
    }

    private static List<Element> elements(Document document, String name) {
        NodeList nodes = document.getElementsByTagName(name);
        List<Element> list = new ArrayList<Element>();
        for (int i = 0; i < nodes.getLength(); i++) {
            list.add((Element) nodes.item(i));
        }
        return list;
    }

    private static String attribute(Element element, String name) {
        if (!element.hasAttribute(name)) {
            return null;
        }
        return element.getAttribute(name);
    }

    private static byte[] readAll(File file) throws IOException {
        FileInputStream in = new FileInputStream(file);
        try {
            byte[] body = new byte[(int) file.length()];
            int offset = 0;
            while (offset < body.length) {
                int n = in.read(body, offset, body.length - offset);
                if (n < 0) {
                    break;
                }
                offset += n;
            }
            return body;
        } finally {
            in.close();
        }
    }

    private static void send(HttpExchange exchange, int status, String type, byte[] body) throws IOException {
        exchange.getResponseHeaders().set("Content-Type", type);
        exchange.sendResponseHeaders(status, body.length);
        try (OutputStream out = exchange.getResponseBody()) {
            out.write(body);
        }
    }

    private static void restore(String key, String value) {
        if (value == null) {
            System.clearProperty(key);
        } else {
            System.setProperty(key, value);
        }
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        file.delete();
    }

    private static void pass(String name) {
        System.out.println("PASS " + name);
    }

    private static void fail(String name, String detail) {
        failures++;
        System.out.println("FAIL " + name + " — " + detail);
    }
}
