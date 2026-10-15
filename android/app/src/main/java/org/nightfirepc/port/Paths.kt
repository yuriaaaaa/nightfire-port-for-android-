package org.nightfirepc.port

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import java.io.File

/**
 * As pastas do jogador e as preferências do lançador.
 *
 * Internal storage/Nightfire é a pasta do jogo no aparelho: o ISO (ou a pasta extraída do disco) pode ficar ali, e o
 * jogo grava ali nightfire.ini, saves/ e logs/ (NF_FILES_DIR, NF_LOG_FILE: platform/entry_sdl.cpp). Nada do disco é
 * copiado para dentro do app: o jogo lê o ISO onde ele está (platform/disc_source.h).
 */
object Paths {
    private const val PREFS = "nightfire"
    private const val KEY_DISC = "disc"
    const val KEY_TOUCH = "touch"
    const val KEY_TOUCH_ALPHA = "touch_alpha"
    const val KEY_LOOK_SENS = "look_sens"
    const val KEY_GYRO = "gyro"
    const val KEY_GYRO_SENS = "gyro_sens"
    private const val KEY_ASSETS_VERSION = "assets_version"

    fun root(): File = File(Environment.getExternalStorageDirectory(), "Nightfire")
    fun logs(): File = File(root(), "logs")

    fun prefs(c: Context): SharedPreferences = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** O disco escolhido, se ainda existe; senão o primeiro que a pasta Nightfire tiver. */
    fun disc(c: Context): String? {
        prefs(c).getString(KEY_DISC, null)?.let { if (isDisc(File(it))) return it }
        return findDisc(root())?.absolutePath
    }

    fun setDisc(c: Context, path: String) = prefs(c).edit().putString(KEY_DISC, path).apply()

    /** Um ISO (ou .cdr) ou uma pasta com os arquivos do disco (SYSTEM.CNF, ou ACTION.ELF e FILES.BIN). */
    fun isDisc(f: File): Boolean {
        if (f.isFile) return f.name.lowercase().let { it.endsWith(".iso") || it.endsWith(".cdr") }
        if (!f.isDirectory) return false
        val names = f.list()?.map { it.uppercase() } ?: return false
        return "SYSTEM.CNF" in names || ("ACTION.ELF" in names && "FILES.BIN" in names)
    }

    private fun findDisc(dir: File): File? {
        if (!dir.isDirectory) return null
        if (isDisc(dir)) return dir
        val children = dir.listFiles()?.sortedBy { it.name.lowercase() } ?: return null
        children.firstOrNull { it.isFile && isDisc(it) }?.let { return it }
        return children.firstOrNull { it.isDirectory && it.name != "logs" && it.name != "saves" && isDisc(it) }
    }

    /**
     * O caminho de um documento do Storage Access Framework, para o jogo abrir por POSIX (com "Acesso a todos os
     * arquivos"): "primary:x" é o armazenamento interno, "XXXX-XXXX:x" um cartão, "raw:/x" já é um caminho.
     */
    fun pathOf(uri: Uri): String? {
        val id = try {
            if (DocumentsContract.isTreeUri(uri) && !DocumentsContract.isDocumentUri(null, uri))
                DocumentsContract.getTreeDocumentId(uri)
            else DocumentsContract.getDocumentId(uri)
        } catch (e: Exception) {
            return null
        }
        if (id.startsWith("raw:")) return id.substring(4)
        val colon = id.indexOf(':')
        if (colon < 0) return null
        val volume = id.substring(0, colon)
        val rest = id.substring(colon + 1)
        val base = if (volume.equals("primary", true)) Environment.getExternalStorageDirectory().absolutePath
        else "/storage/$volume"
        return if (rest.isEmpty()) base else "$base/$rest"
    }

    /** Copia a pasta hd_ui do APK para Internal storage/Nightfire/hd_ui quando a versão do app muda. */
    fun installAssets(c: Context, versionCode: Long) {
        val p = prefs(c)
        val dest = File(root(), "hd_ui")
        if (p.getLong(KEY_ASSETS_VERSION, -1) == versionCode && File(dest, "map.json").isFile) return
        copyAssetDir(c, "hd_ui", dest)
        p.edit().putLong(KEY_ASSETS_VERSION, versionCode).apply()
    }

    private fun copyAssetDir(c: Context, path: String, dest: File) {
        val list = c.assets.list(path) ?: return
        if (list.isEmpty()) {
            dest.parentFile?.mkdirs()
            c.assets.open(path).use { input -> dest.outputStream().use { input.copyTo(it) } }
            return
        }
        dest.mkdirs()
        for (name in list) copyAssetDir(c, "$path/$name", File(dest, name))
    }

    /**
     * O nightfire.ini de um jogador novo: o estilo de botões NightFire (o analógico esquerdo anda e o direito olha,
     * como os jogos de toque) com os gatilhos atirando e mirando, e sem a tela de entrada do PC (o lançador faz o
     * papel dela). Um nightfire.ini que já existe não é tocado.
     */
    fun writeDefaultIni() {
        val ini = File(root(), "nightfire.ini")
        if (ini.exists()) return
        root().mkdirs()
        ini.writeText(
            """
            ; Nightfire PC (Android). O jogo regrava este arquivo quando uma opção muda nos menus.
            [ui]
            launcher = 0

            [controls]
            pad_style = 0
            pad_shoulders = triggers

            """.trimIndent()
        )
    }

    private fun runningMarker(): File = File(logs(), ".running")

    /** Marca que uma partida começou (GameActivity); a próxima abertura do lançador acha a marca e guarda o logcat. */
    fun markRunning() {
        try {
            logs().mkdirs()
            runningMarker().writeText(System.currentTimeMillis().toString())
        } catch (e: Exception) {
        }
    }

    /**
     * Se a última partida não terminou pelo menu do lançador (a marca ficou), grava o logcat do app (os registros do
     * próprio usuário do app, que o Android deixa ler: o motivo de uma queda do processo, a pilha do Java ou a do
     * código nativo) em logs/logcat-<data>.txt. Devolve o arquivo, ou null.
     */
    fun saveLogcatAfterCrash(): File? {
        val marker = runningMarker()
        if (!marker.isFile) return null
        val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
            .format(java.util.Date(marker.lastModified()))
        marker.delete()
        val out = File(logs(), "logcat-$stamp.txt")
        return try {
            val p = ProcessBuilder("logcat", "-d", "-v", "threadtime", "-b", "main", "-b", "crash")
                .redirectErrorStream(true).start()
            out.outputStream().use { p.inputStream.copyTo(it) }
            p.waitFor()
            dropOldFiles("logcat-", ".txt")
            out
        } catch (e: Exception) {
            null
        }
    }

    fun clearRunning() {
        runningMarker().delete()
    }

    private fun dropOldFiles(prefix: String, suffix: String) {
        logs().listFiles { f -> f.name.startsWith(prefix) && f.name.endsWith(suffix) }
            ?.sortedByDescending { it.name }?.drop(8)?.forEach { it.delete() }
    }

    /** Guarda os registros antigos (os 8 mais novos) e devolve o caminho do desta partida. */
    fun newLogFile(): File {
        val dir = logs()
        dir.mkdirs()
        val current = File(dir, "nightfire.log")
        if (current.isFile && current.length() > 0) {
            val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
                .format(java.util.Date(current.lastModified()))
            current.renameTo(File(dir, "nightfire-$stamp.log"))
        }
        dir.listFiles { f -> f.name.startsWith("nightfire-") && f.name.endsWith(".log") }
            ?.sortedByDescending { it.name }?.drop(8)?.forEach { it.delete() }
        return current
    }
}
