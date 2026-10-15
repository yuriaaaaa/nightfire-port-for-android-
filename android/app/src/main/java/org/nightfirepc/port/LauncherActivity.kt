package org.nightfirepc.port

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.CompoundButton
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import java.io.File

/**
 * A tela de entrada do Android: o acesso ao armazenamento, o disco (o ISO do jogador ou a pasta com os arquivos do
 * disco), as opções do toque e JOGAR. As opções do jogo (vídeo, controles, teclas) ficam nos menus do próprio jogo.
 */
class LauncherActivity : Activity() {

    private val gold = Color.rgb(0xd4, 0xa9, 0x3b)
    private lateinit var storageText: TextView
    private lateinit var discText: TextView
    private lateinit var playButton: Button

    private val pickIso = 1
    private val pickFolder = 2

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.decorView.setBackgroundColor(Color.rgb(0x0e, 0x0c, 0x0a))
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(28), dp(24), dp(28), dp(24))
        }
        col.addView(TextView(this).apply {
            text = "007 NIGHTFIRE"
            textSize = 30f
            setTextColor(gold)
            typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
            letterSpacing = 0.15f
        })
        col.addView(label("PC port for Android · Vulkan · ${versionName()}", 13f, Color.GRAY))
        col.addView(space(16))

        storageText = label("", 14f, Color.LTGRAY)
        col.addView(storageText)
        col.addView(button("Allow storage access") { requestStorage() })
        col.addView(space(12))

        col.addView(section("DISC"))
        discText = label("", 14f, Color.LTGRAY)
        col.addView(discText)
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        row.addView(button("Choose ISO") { pick(pickIso) }, LinearLayout.LayoutParams(0, -2, 1f))
        row.addView(space(8))
        row.addView(button("Choose folder") { pick(pickFolder) }, LinearLayout.LayoutParams(0, -2, 1f))
        col.addView(row)
        col.addView(label("Use your own copy of 007: Nightfire for PS2 (NTSC-U, SLUS-20579). Put the .iso in " +
            "Internal storage/Nightfire to have it found on its own.", 12f, Color.GRAY))
        col.addView(space(12))

        col.addView(section("CONTROLS"))
        val prefs = Paths.prefs(this)
        col.addView(toggle("Touch controls (4 fingers open the menu)", prefs.getBoolean(Paths.KEY_TOUCH, true)) {
            prefs.edit().putBoolean(Paths.KEY_TOUCH, it).apply()
        })
        col.addView(toggle("Gyro aim", prefs.getBoolean(Paths.KEY_GYRO, false)) {
            prefs.edit().putBoolean(Paths.KEY_GYRO, it).apply()
        })
        col.addView(label("A Bluetooth or USB controller works too; using it hides the touch controls.", 12f, Color.GRAY))
        col.addView(space(16))

        playButton = button("PLAY") { play() }.apply { textSize = 20f }
        col.addView(playButton)
        col.addView(space(8))
        col.addView(label("Settings, saves and logs: Internal storage/Nightfire (logs/nightfire.log).", 12f, Color.GRAY))

        setContentView(ScrollView(this).apply { addView(col) })
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun hasStorage(): Boolean =
        if (Build.VERSION.SDK_INT >= 30) Environment.isExternalStorageManager()
        else checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED

    private fun requestStorage() {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                startActivity(Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:$packageName")))
            } catch (e: Exception) {
                startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            }
        } else {
            requestPermissions(arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE,
                Manifest.permission.WRITE_EXTERNAL_STORAGE), 3)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        refresh()
    }

    private fun refresh() {
        val ok = hasStorage()
        storageText.text = if (ok) "Storage access: allowed" else "Storage access is needed to read your ISO and keep saves."
        if (ok) {
            Paths.root().mkdirs()
            Paths.logs().mkdirs()
            checkLastRun()
        }
        val disc = if (ok) Paths.disc(this) else null
        discText.text = when {
            !ok -> "Disc: allow storage access first"
            disc == null -> "Disc: not found"
            else -> "Disc: $disc"
        }
        playButton.isEnabled = ok && disc != null
        playButton.alpha = if (playButton.isEnabled) 1f else 0.4f
    }

    // A última partida caiu (a marca de GameActivity ficou): guarda o logcat dela ao lado do nightfire.log.
    private fun checkLastRun() {
        Thread {
            val f = Paths.saveLogcatAfterCrash() ?: return@Thread
            runOnUiThread {
                Toast.makeText(this, "The last game stopped. Logs saved in Internal storage/Nightfire/logs " +
                    "(nightfire.log and ${f.name}).", Toast.LENGTH_LONG).show()
            }
        }.start()
    }

    private fun pick(kind: Int) {
        val i = if (kind == pickIso) Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        } else Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
        try {
            startActivityForResult(i, kind)
        } catch (e: Exception) {
            Toast.makeText(this, "No file picker: put the ISO in Internal storage/Nightfire", Toast.LENGTH_LONG).show()
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data ?: return
        if (resultCode != RESULT_OK) return
        val path = Paths.pathOf(uri)
        if (path == null || !Paths.isDisc(File(path))) {
            Toast.makeText(this, "That is not a Nightfire disc image or disc folder" +
                (path?.let { ":\n$it" } ?: ""), Toast.LENGTH_LONG).show()
            return
        }
        Paths.setDisc(this, path)
        refresh()
    }

    private fun play() {
        if (!hasStorage()) return requestStorage()
        val disc = Paths.disc(this) ?: return refresh()
        try {
            Paths.writeDefaultIni()
            @Suppress("DEPRECATION")
            val code = packageManager.getPackageInfo(packageName, 0).let {
                if (Build.VERSION.SDK_INT >= 28) it.longVersionCode else it.versionCode.toLong()
            }
            Paths.installAssets(this, code)
        } catch (e: Exception) {
            Toast.makeText(this, "Could not prepare Internal storage/Nightfire: $e", Toast.LENGTH_LONG).show()
        }
        startActivity(Intent(this, GameActivity::class.java).putExtra(GameActivity.EXTRA_DISC, disc))
    }

    private fun versionName(): String = try {
        packageManager.getPackageInfo(packageName, 0).versionName ?: ""
    } catch (e: Exception) {
        ""
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun space(h: Int) = View(this).apply { minimumHeight = dp(h); minimumWidth = dp(h) }

    private fun label(t: String, size: Float, color: Int) = TextView(this).apply {
        text = t
        textSize = size
        setTextColor(color)
        setPadding(0, dp(2), 0, dp(2))
    }

    private fun section(t: String) = label(t, 13f, gold).apply {
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
        letterSpacing = 0.2f
        setPadding(0, dp(6), 0, dp(4))
    }

    private fun button(t: String, onClick: () -> Unit) = Button(this).apply {
        text = t
        isAllCaps = true
        setTextColor(Color.WHITE)
        gravity = Gravity.CENTER
        background = GradientDrawable().apply {
            setColor(Color.rgb(0x22, 0x1c, 0x14))
            setStroke(dp(1), gold)
            cornerRadius = dp(4).toFloat()
        }
        setOnClickListener { onClick() }
    }

    private fun toggle(t: String, on: Boolean, changed: (Boolean) -> Unit) = Switch(this).apply {
        text = t
        isChecked = on
        setTextColor(Color.LTGRAY)
        setPadding(0, dp(6), 0, dp(6))
        setOnCheckedChangeListener { _: CompoundButton, v: Boolean -> changed(v) }
    }
}
