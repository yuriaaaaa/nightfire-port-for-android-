package org.nightfirepc.port

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot
import kotlin.math.min

/**
 * O controle de toque sobre o jogo. Vira um controle do Xbox virtual (os bits do XInput, platform/gamepad.h) que entra
 * no controle 0 do jogo pela JNI (jni_android.cpp), então vale o estilo de botões escolhido no jogo; no padrão do
 * lançador, o estilo NightFire: RT atira, LT mira, Y pula, A agacha, X usa e recarrega, B troca o modo de tiro, Back a
 * visão, o direcional troca as armas (esquerda/direita) e os aparelhos (cima/baixo), Start pausa.
 *
 * O lado esquerdo é um analógico que nasce onde o dedo encosta; arrastar o lado direito (ou o dedo que segura ATIRAR ou
 * MIRAR) olha, como um mouse (psiInput_AddTouchLook). Quatro dedos juntos abrem o menu do lançador.
 */
class VirtualPadView(context: Context, private val onMenu: () -> Unit) : View(context) {

    companion object {
        const val DPAD_UP = 0x0001
        const val DPAD_DOWN = 0x0002
        const val DPAD_LEFT = 0x0004
        const val DPAD_RIGHT = 0x0008
        const val START = 0x0010
        const val BACK = 0x0020
        const val LB = 0x0100
        const val RB = 0x0200
        const val A = 0x1000
        const val B = 0x2000
        const val X = 0x4000
        const val Y = 0x8000
        // Os gatilhos vão como bytes à parte; estes bits só os distinguem aqui.
        const val LT = 0x10000
        const val RT = 0x20000
    }

    private class Button(val label: String, val bits: Int, val looks: Boolean = false) {
        var cx = 0f
        var cy = 0f
        var r = 0f
        fun hit(x: Float, y: Float) = hypot(x - cx, y - cy) <= r * 1.15f
    }

    private val buttons = listOf(
        Button("FIRE", RT, looks = true),
        Button("AIM", LT, looks = true),
        Button("JUMP", Y),
        Button("CROUCH", A),
        Button("USE", X),
        Button("MODE", B),
        Button("▲", DPAD_UP),
        Button("▼", DPAD_DOWN),
        Button("◀", DPAD_LEFT),
        Button("▶", DPAD_RIGHT),
        Button("START", START),
        Button("VISION", BACK),
    )

    var lookSensitivity = 1.0f
    var opacity = 0.55f
        set(v) {
            field = v
            invalidate()
        }
    var active = true
        set(v) {
            field = v
            if (!v) releaseAll()
            visibility = if (v) VISIBLE else INVISIBLE
            send()
        }

    // Um dedo: o que ele segura e onde estava (para o olhar).
    private class Finger(var button: Button?, var stick: Boolean, var lastX: Float, var lastY: Float)

    private val fingers = HashMap<Int, Finger>()
    private var stickX = 0f
    private var stickY = 0f
    private var stickDX = 0f
    private var stickDY = 0f
    private var stickR = 0f
    private var lastButtons = -1
    private var lastLx = 0
    private var lastLy = 0
    private var menuArmed = true

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        val u = min(w, h) / 7f
        stickR = 1.15f * u
        fun place(label: String, x: Float, y: Float, r: Float) {
            buttons.first { it.label == label }.apply { cx = x; cy = y; this.r = r }
        }
        place("FIRE", w - 1.9f * u, h - 2.0f * u, 0.95f * u)
        place("AIM", w - 4.1f * u, h - 1.15f * u, 0.7f * u)
        place("JUMP", w - 1.0f * u, h - 4.15f * u, 0.55f * u)
        place("CROUCH", w - 2.45f * u, h - 3.9f * u, 0.55f * u)
        place("USE", w - 3.7f * u, h - 3.0f * u, 0.55f * u)
        place("MODE", w - 0.75f * u, h - 5.5f * u, 0.42f * u)
        val dx = 1.55f * u
        val dy = 1.55f * u
        place("▲", dx, dy - 0.78f * u, 0.4f * u)
        place("▼", dx, dy + 0.78f * u, 0.4f * u)
        place("◀", dx - 0.78f * u, dy, 0.4f * u)
        place("▶", dx + 0.78f * u, dy, 0.4f * u)
        place("START", w / 2f + 0.9f * u, 0.55f * u, 0.38f * u)
        place("VISION", w / 2f - 0.9f * u, 0.55f * u, 0.38f * u)
        text.textSize = 0.26f * u
        stroke.strokeWidth = 0.04f * u
    }

    private fun buttonAt(x: Float, y: Float) = buttons.firstOrNull { it.hit(x, y) }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (!active) return false
        if (e.pointerCount >= 4) {
            if (menuArmed) {
                menuArmed = false
                releaseAll()
                send()
                onMenu()
            }
            return true
        }
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = e.actionIndex
                val x = e.getX(i)
                val y = e.getY(i)
                val b = buttonAt(x, y)
                val stick = b == null && x < width * 0.4f && fingers.values.none { it.stick }
                if (stick) {
                    stickX = x
                    stickY = y
                    stickDX = 0f
                    stickDY = 0f
                }
                if (b != null) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                fingers[e.getPointerId(i)] = Finger(b, stick, x, y)
            }
            MotionEvent.ACTION_MOVE -> {
                var lookX = 0f
                var lookY = 0f
                for (i in 0 until e.pointerCount) {
                    val f = fingers[e.getPointerId(i)] ?: continue
                    val x = e.getX(i)
                    val y = e.getY(i)
                    if (f.stick) {
                        var ddx = x - stickX
                        var ddy = y - stickY
                        val d = hypot(ddx, ddy)
                        if (d > stickR) {
                            // O analógico segue o dedo que passa da borda.
                            stickX += ddx * (1 - stickR / d)
                            stickY += ddy * (1 - stickR / d)
                            ddx = x - stickX
                            ddy = y - stickY
                        }
                        stickDX = ddx / stickR
                        stickDY = ddy / stickR
                    } else if (f.button == null || f.button!!.looks) {
                        lookX += x - f.lastX
                        lookY += y - f.lastY
                    }
                    f.lastX = x
                    f.lastY = y
                }
                if (lookX != 0f || lookY != 0f) NativeBridge.nativeAddTouchLook(lookX * lookSensitivity, lookY * lookSensitivity)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                if (e.actionMasked == MotionEvent.ACTION_CANCEL) {
                    fingers.clear()
                } else {
                    val f = fingers.remove(e.getPointerId(e.actionIndex))
                    if (f?.stick == true) {
                        stickDX = 0f
                        stickDY = 0f
                    }
                }
                if (fingers.isEmpty()) {
                    menuArmed = true
                    stickDX = 0f
                    stickDY = 0f
                }
            }
        }
        send()
        invalidate()
        return true
    }

    private fun releaseAll() {
        fingers.clear()
        stickDX = 0f
        stickDY = 0f
        invalidate()
    }

    private fun held(): Int = fingers.values.fold(0) { acc, f -> acc or (f.button?.bits ?: 0) }

    private fun send() {
        val b = if (active) held() else 0
        val lx = (stickDX.coerceIn(-1f, 1f) * 32767).toInt()
        val ly = (-stickDY.coerceIn(-1f, 1f) * 32767).toInt()
        if (b == lastButtons && lx == lastLx && ly == lastLy) return
        lastButtons = b
        lastLx = lx
        lastLy = ly
        NativeBridge.nativeSetVirtualPadState(
            active, b and 0xffff, lx, ly, 0, 0,
            if (b and LT != 0) 255 else 0, if (b and RT != 0) 255 else 0
        )
    }

    override fun onDraw(c: Canvas) {
        if (!active) return
        val a = (opacity * 255).toInt().coerceIn(20, 255)
        val held = held()
        for (b in buttons) {
            val on = held and b.bits != 0
            fill.color = if (on) 0xd4a93b else 0x101010
            fill.alpha = if (on) a else a / 2
            c.drawCircle(b.cx, b.cy, b.r, fill)
            stroke.color = 0xd4a93b
            stroke.alpha = a
            c.drawCircle(b.cx, b.cy, b.r, stroke)
            text.color = 0xffffff
            text.alpha = a
            c.drawText(b.label, b.cx, b.cy - (text.descent() + text.ascent()) / 2, text)
        }
        if (fingers.values.any { it.stick }) {
            stroke.color = 0xd4a93b
            stroke.alpha = a
            c.drawCircle(stickX, stickY, stickR, stroke)
            fill.color = 0xd4a93b
            fill.alpha = a
            c.drawCircle(stickX + stickDX.coerceIn(-1f, 1f) * stickR, stickY + stickDY.coerceIn(-1f, 1f) * stickR,
                stickR * 0.4f, fill)
        }
    }
}
