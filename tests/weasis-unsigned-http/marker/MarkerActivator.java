package marker;

import java.io.FileWriter;
import java.io.IOException;

import org.osgi.framework.BundleActivator;
import org.osgi.framework.BundleContext;

/**
 * Substituted bundle. Writing the marker is the side effect that must not
 * happen once the launcher rejects an unauthenticated jar.
 */
public class MarkerActivator implements BundleActivator {
    @Override
    public void start(BundleContext context) throws IOException {
        String path = System.getProperty("weasis.test.marker");
        FileWriter writer = new FileWriter(path);
        try {
            writer.write("executed");
        } finally {
            writer.close();
        }
    }

    @Override
    public void stop(BundleContext context) {
    }
}
