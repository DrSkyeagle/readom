import java.security.KeyStore;
import java.security.Key;
import java.util.Arrays;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.spec.GCMParameterSpec;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

/** Uses the real Android keystore service and software Keymaster in RAM only. */
public final class ServiceSmoke {
    private static int checks;
    private static void check(boolean ok, String name) {
        if (!ok) throw new AssertionError(name);
        checks++;
        System.out.println("SERVICE_CHECK " + name + " PASS");
    }
    public static void main(String[] args) throws Exception {
        android.os.Looper.prepareMainLooper();
        Class.forName("android.app.ActivityThread").getMethod("systemMain").invoke(null);
        Class.forName("android.security.keystore.AndroidKeyStoreProvider")
            .getMethod("install").invoke(null);
        final String alias = "pw3-isolated-service-smoke";
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        check(!store.containsAlias(alias), "fresh-ram-keystore");
        KeyGenerator generator = KeyGenerator.getInstance("AES", "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(alias,
            KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());
        Key generated = generator.generateKey();
        check(store.containsAlias(alias), "key-generated-through-service");
        check(generated.getEncoded() == null, "key-material-not-exported");
        Key loaded = store.getKey(alias, null);
        check(loaded != null, "key-reloaded-by-alias");
        byte[] plaintext = "PW3 isolated RAM service roundtrip".getBytes("UTF-8");
        Cipher encrypt = Cipher.getInstance("AES/GCM/NoPadding");
        encrypt.init(Cipher.ENCRYPT_MODE, loaded);
        byte[] ciphertext = encrypt.doFinal(plaintext);
        Cipher decrypt = Cipher.getInstance("AES/GCM/NoPadding");
        decrypt.init(Cipher.DECRYPT_MODE, loaded, new GCMParameterSpec(128, encrypt.getIV()));
        check(Arrays.equals(plaintext, decrypt.doFinal(ciphertext)), "aes-gcm-roundtrip");
        ciphertext[0] ^= 1;
        Cipher tampered = Cipher.getInstance("AES/GCM/NoPadding");
        tampered.init(Cipher.DECRYPT_MODE, loaded, new GCMParameterSpec(128, encrypt.getIV()));
        boolean rejected = false;
        try { tampered.doFinal(ciphertext); }
        catch (javax.crypto.AEADBadTagException expected) { rejected = true; }
        check(rejected, "tampered-ciphertext-rejected");
        store.deleteEntry(alias);
        check(!store.containsAlias(alias), "key-deleted");
        System.out.println("PW3_AOSP_SERVICE_SMOKE_PASS checks=" + checks);
    }
}
