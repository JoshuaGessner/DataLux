package dev.fileflow

import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import dev.fileflow.probe.CapabilityProbe

/**
 * The app's front door.
 *
 * ## Why this exists
 *
 * Until now every screen was reachable only through `adb shell am start`, and the launcher pointed at
 * the capability probe — so a person holding the phone got a wall of monospace diagnostics and no way
 * to reach the transmitter, the receiver, or the aiming help. During a real two-device test the
 * receiver showed the capture report while the operator was trying to line up a camera, which is the
 * opposite of useful.
 *
 * The three buttons are the three things a person actually does. Everything remains scriptable: the
 * activities still take the same intent extras, and this screen only supplies sensible defaults.
 *
 * ## The grid is chosen here, not assumed
 *
 * The grid must have an integer cell pitch on the DEVICE THAT TRANSMITS, and that is device-dependent
 * (DEVICE-MATRIX). Worse, an app does not necessarily get the panel's native resolution — a request
 * for 1440×3120 came back as 1080×2340 on the reference device, which made the charter grid fractional
 * (F31). So the transmitter picks its own pitch at runtime from the surface it is actually given, and
 * the choice offered here is a *grid*, with the pitch left to the renderer.
 */
class MainActivity : AppCompatActivity() {

    private var cols = 120
    private var rows = 260

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.parseColor("#101418"))
            setPadding(48, 72, 48, 48)
        }

        root.addView(heading("FileFlow"))
        root.addView(
            body(
                "Phone-to-phone file transfer over light alone — no Wi-Fi, Bluetooth, NFC or cable. " +
                    "One screen transmits, one camera receives."
            )
        )

        // An honest status line. The probe returns UNSUPPORTED on every device today because nothing
        // has been VERIFIED by measurement, and that is the correct answer rather than a defect
        // (RISK-011). Saying so here is better than letting a user infer the app is broken.
        root.addView(spacer(24))
        root.addView(body(statusLine()))
        root.addView(spacer(32))

        root.addView(gridChooser())
        root.addView(spacer(32))

        root.addView(
            bigButton("Send", "#2E7D32") {
                startActivity(
                    Intent(this, TransmitActivity::class.java)
                        .putExtra("cols", cols)
                        .putExtra("rows", rows)
                        .putExtra("divisor", 8)
                        .putExtra("seconds", 120)
                )
            }
        )
        root.addView(caption("Shows the optical frames. Point the other phone's camera at this screen."))

        root.addView(spacer(20))
        root.addView(
            bigButton("Receive", "#1565C0") {
                // Aiming is ON by default here. A person holding a camera needs help pointing it far
                // more than they need a diagnostics dump, and receiving starts itself once the
                // geometry is workable.
                //
                // LIVE: decode on the phone until the file verifies, with progress on screen (C13b).
                // This button used to record 120 frames -- four seconds -- for desktop analysis, which
                // could not deliver a file and could not say whether anything had worked. Scripted
                // experiments still get the recording path: they launch CaptureActivity without
                // `live`.
                startActivity(
                    Intent(this, CaptureActivity::class.java)
                        .putExtra("gridCols", cols)
                        .putExtra("gridRows", rows)
                        .putExtra("aim", true)
                        .putExtra("live", true)
                        .putExtra("fps", 30)
                        .putExtra("maxWidth", 2688)
                        .putExtra("exposureNs", 16_000_000L)
                )
            }
        )
        root.addView(caption("Lines up the camera, then receives until the file is verified. " +
                             "If the phone drifts, it pauses — nothing received is lost."))

        root.addView(spacer(20))
        root.addView(
            bigButton("Check this device", "#455A64") {
                startActivity(Intent(this, ProbeActivity::class.java))
            }
        )
        root.addView(caption("What the camera and display claim they can do, and what that is worth."))

        root.addView(spacer(36))
        root.addView(
            caption(
                "Nothing here measures goodput. Fd, Pc and the density limit are all still " +
                    "unmeasured on real hardware, so no rate this app prints is a result."
            )
        )

        setContentView(ScrollView(this).apply {
            setBackgroundColor(Color.parseColor("#101418"))
            addView(root)
        })
    }

    /** Grid choice, offered as the two options DEVICE-MATRIX recommends plus a denser one. */
    private fun gridChooser(): View {
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val label = body("Grid")
        val options = listOf(
            120 to 260,   // square cells on a 1080x2340 surface, integer at 1440x3120 too (F31)
            144 to 240,   // the charter grid; integer only at the panel's native resolution
            180 to 390,   // densest integer-pitch grid the reference panel permits
        )
        val buttons = ArrayList<Button>()
        options.forEach { (c, r) ->
            val b = Button(this).apply {
                text = "${c}x$r"
                textSize = 15f
                setOnClickListener {
                    cols = c
                    rows = r
                    buttons.forEach { it.alpha = 0.45f }
                    alpha = 1.0f
                }
            }
            buttons.add(b)
            row.addView(b, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        }
        buttons.forEachIndexed { i, b -> b.alpha = if (i == 0) 1.0f else 0.45f }

        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(label)
            addView(row)
            addView(
                caption(
                    "Denser is faster only until the receiver cannot resolve it. The transmitter " +
                        "picks an exact integer pitch for whatever surface it is given."
                )
            )
        }
    }

    private fun statusLine(): String = runCatching {
        val (claims, modes) = CapabilityProbe.readClaims(this)
        val hs = modes.count { it.highSpeed }
        val cpu = modes.filter { it.cpuReadable }.maxOfOrNull { it.maxFps } ?: 0.0
        "This phone: ${claims.panelWidth}x${claims.panelHeight} at ${claims.maxRefreshHz.toInt()} Hz, " +
            "camera up to ${cpu.toInt()} fps readable" + if (hs > 0) ", $hs high-speed mode(s)." else "."
    }.getOrElse { "This phone: capabilities unavailable (${it.javaClass.simpleName})." }

    // ---------------------------------------------------------------- small view helpers

    private fun heading(s: String) = TextView(this).apply {
        text = s
        textSize = 34f
        setTextColor(Color.WHITE)
        setTypeface(typeface, android.graphics.Typeface.BOLD)
    }

    private fun body(s: String) = TextView(this).apply {
        text = s
        textSize = 16f
        setTextColor(Color.parseColor("#CFD8DC"))
    }

    private fun caption(s: String) = TextView(this).apply {
        text = s
        textSize = 13f
        setTextColor(Color.parseColor("#78909C"))
        setPadding(4, 6, 4, 0)
    }

    private fun spacer(h: Int) = View(this).apply {
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, h)
    }

    private fun bigButton(label: String, colour: String, onClick: () -> Unit) = Button(this).apply {
        text = label
        textSize = 20f
        setTextColor(Color.WHITE)
        setBackgroundColor(Color.parseColor(colour))
        gravity = Gravity.CENTER
        setPadding(0, 36, 0, 36)
        setOnClickListener { onClick() }
    }
}
