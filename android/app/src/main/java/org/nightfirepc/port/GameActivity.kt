package org.nightfirepc.port

import android.app.AlertDialog
import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Bundle
import android.system.Os
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import org.libsdl.app.SDLActivity

/**
 * O jogo: o SDLActivity do SDL2 carrega libmain.so (o CMakeLists.txt da raiz) e chama o SDL_main dele
 * (platform/entry_sdl.cpp) com "--disc <o disco>", o que pula a tela de entrada do PC. Por cima, o controle de toque
 * (VirtualPadView) e, se ligado, a mira pelo giroscópio. Quatro dedos abrem o menu.
 */
class GameActivity : SDLActivity(), SensorEventListener {

    companion object {
        const val EXTRA_DISC = "disc"
    }

    private var pad: VirtualPadView? = null
    private var sensors: SensorManager? = null
    private var gyroOn = false
    private var gyroSens = 1.0f
    private var lastGyroNs = 0L
    private var menu: AlertDialog? = null

    // O SDL2 e o libc++ estão dentro da libmain.so (CMakeLists.txt).
    override fun getLibraries() = arrayOf("main")

    // Antes do System.loadLibrary("main") do SDL: o carregador abre a libmain.so no começo de uma reserva de 4 GB
    // (NativeLoader); o loadLibrary do SDL acha a mesma biblioteca já aberta e o ART chama o JNI_OnLoad dela.
    override fun loadLibraries() {
        NativeLoader.preload()
        super.loadLibraries()
    }

    override fun getArguments(): Array<String> {
        val disc = intent.getStringExtra(EXTRA_DISC) ?: return emptyArray()
        return arrayOf("--disc", disc)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // O SDL_main lê estes dois antes de tudo (platform/entry_sdl.cpp): o registro desta partida e a pasta onde o
        // jogo grava nightfire.ini e saves/.
        try {
            Paths.root().mkdirs()
            Os.setenv("NF_FILES_DIR", Paths.root().absolutePath, true)
            Os.setenv("NF_LOG_FILE", Paths.newLogFile().absolutePath, true)
            Paths.markRunning()
        } catch (e: Exception) {
            android.util.Log.w("Nightfire", "setenv: $e")
        }
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (mBrokenLibraries) return
        val prefs = Paths.prefs(this)
        val v = VirtualPadView(this) { showMenu() }
        v.lookSensitivity = prefs.getFloat(Paths.KEY_LOOK_SENS, 1.0f)
        v.opacity = prefs.getFloat(Paths.KEY_TOUCH_ALPHA, 0.55f)
        mLayout.addView(v, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        v.active = prefs.getBoolean(Paths.KEY_TOUCH, true)
        pad = v
        gyroOn = prefs.getBoolean(Paths.KEY_GYRO, false)
        gyroSens = prefs.getFloat(Paths.KEY_GYRO_SENS, 1.0f)
        sensors = getSystemService(Context.SENSOR_SERVICE) as SensorManager
    }

    override fun onResume() {
        super.onResume()
        updateGyro()
    }

    override fun onPause() {
        sensors?.unregisterListener(this)
        super.onPause()
    }

    private fun updateGyro() {
        val s = sensors ?: return
        s.unregisterListener(this)
        lastGyroNs = 0L
        if (!gyroOn) return
        s.getDefaultSensor(Sensor.TYPE_GYROSCOPE)?.let { s.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME) }
    }

    // A mira pelo giroscópio: a velocidade angular vezes o tempo dá o giro em radianos, que vira contagens do mouse na
    // sensibilidade padrão do jogo (0,0022 rad por contagem), então "1,0" gira a câmera tanto quanto o aparelho.
    override fun onSensorChanged(e: SensorEvent) {
        if (e.sensor.type != Sensor.TYPE_GYROSCOPE) return
        val last = lastGyroNs
        lastGyroNs = e.timestamp
        if (last == 0L) return
        val dt = (e.timestamp - last) * 1e-9f
        if (dt <= 0f || dt > 0.1f) return
        @Suppress("DEPRECATION")
        val rotation = windowManager.defaultDisplay.rotation
        val sign = if (rotation == Surface.ROTATION_270) -1f else 1f
        val k = gyroSens * dt / 0.0022f
        NativeBridge.nativeAddTouchLook(-sign * e.values[0] * k, sign * e.values[1] * k)
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    // Um controle físico esconde o de toque; tocar a tela de novo o traz de volta (VirtualPadView recebe o toque só
    // se estiver ativo, então o toque passa por aqui primeiro).
    private fun fromGamepad(source: Int) =
        source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (fromGamepad(event.source) && pad?.active == true) pad?.active = false
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(ev: MotionEvent): Boolean {
        if (fromGamepad(ev.source) && pad?.active == true) pad?.active = false
        return super.dispatchGenericMotionEvent(ev)
    }

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        val p = pad
        if (p != null && !p.active && ev.actionMasked == MotionEvent.ACTION_DOWN &&
            Paths.prefs(this).getBoolean(Paths.KEY_TOUCH, true)) {
            p.active = true
        }
        return super.dispatchTouchEvent(ev)
    }

    private fun showMenu() {
        if (menu?.isShowing == true) return
        val prefs = Paths.prefs(this)
        val items = arrayOf(
            "Resume",
            "Pause menu (Start)",
            if (pad?.active == true) "Hide touch controls" else "Show touch controls",
            if (gyroOn) "Gyro aim: on" else "Gyro aim: off",
            "Look sensitivity and opacity…",
            "Quit game",
        )
        menu = AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Nightfire")
            .setItems(items) { _, which ->
                when (which) {
                    1 -> pressStart()
                    2 -> {
                        val on = pad?.active != true
                        prefs.edit().putBoolean(Paths.KEY_TOUCH, on).apply()
                        pad?.active = on
                    }
                    3 -> {
                        gyroOn = !gyroOn
                        prefs.edit().putBoolean(Paths.KEY_GYRO, gyroOn).apply()
                        updateGyro()
                    }
                    4 -> showSensitivity()
                    5 -> quit()
                }
            }
            .show()
    }

    private fun pressStart() {
        // Um toque de Start pelo controle virtual: apertado e solto um pouco depois (o jogo lê por tick).
        val visible = pad?.active == true
        NativeBridge.nativeSetVirtualPadState(true, VirtualPadView.START, 0, 0, 0, 0, 0, 0)
        mLayout.postDelayed({ NativeBridge.nativeSetVirtualPadState(visible, 0, 0, 0, 0, 0, 0, 0) }, 120)
    }

    private fun showSensitivity() {
        val prefs = Paths.prefs(this)
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 24, 48, 0)
        }
        fun slider(label: String, key: String, def: Float, max: Float, apply: (Float) -> Unit) {
            val title = TextView(this)
            val bar = SeekBar(this).apply { this.max = 100 }
            val value = prefs.getFloat(key, def)
            bar.progress = (value / max * 100).toInt()
            fun show(v: Float) {
                title.text = "%s: %.2f".format(label, v)
            }
            show(value)
            bar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar, p: Int, fromUser: Boolean) {
                    val v = (p.coerceAtLeast(2) / 100f) * max
                    show(v)
                    prefs.edit().putFloat(key, v).apply()
                    apply(v)
                }
                override fun onStartTrackingTouch(s: SeekBar) {}
                override fun onStopTrackingTouch(s: SeekBar) {}
            })
            box.addView(title)
            box.addView(bar)
        }
        slider("Touch look", Paths.KEY_LOOK_SENS, 1.0f, 3.0f) { pad?.lookSensitivity = it }
        slider("Gyro", Paths.KEY_GYRO_SENS, 1.0f, 3.0f) { gyroSens = it }
        slider("Touch controls opacity", Paths.KEY_TOUCH_ALPHA, 0.55f, 1.0f) { pad?.opacity = it }
        AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Controls")
            .setView(box)
            .setPositiveButton("OK", null)
            .show()
    }

    private fun quit() {
        // O jogo grava as opções e os saves na hora; sair encerra o processo (o jogo tem estado estático, como o
        // SDL_main faz quando volta).
        Paths.clearRunning()
        finishAndRemoveTask()
        android.os.Process.killProcess(android.os.Process.myPid())
    }
}
