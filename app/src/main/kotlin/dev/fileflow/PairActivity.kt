package dev.fileflow

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.ImageFormat
import android.graphics.drawable.GradientDrawable
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.ImageReader
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import com.google.android.material.button.MaterialButton
import com.google.zxing.BarcodeFormat
import com.google.zxing.BinaryBitmap
import com.google.zxing.DecodeHintType
import com.google.zxing.EncodeHintType
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.ReaderException
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader
import com.google.zxing.qrcode.QRCodeWriter
import com.google.zxing.qrcode.decoder.ErrorCorrectionLevel
import dev.fileflow.secure.Pairing
import dev.fileflow.secure.PairingKey
import dev.fileflow.secure.PairingStore
import dev.fileflow.secure.Team
import dev.fileflow.secure.TeamInvite
import dev.fileflow.secure.Teams
import dev.fileflow.ui.Ui.Style
import dev.fileflow.ui.Ui.button
import dev.fileflow.ui.Ui.dp
import dev.fileflow.ui.Ui.helpButton
import dev.fileflow.ui.Ui.matchWidth
import dev.fileflow.ui.Ui.space
import dev.fileflow.ui.Ui.text
import dev.fileflow.ui.Ui.title

/**
 * Pairs two phones for encrypted mode (see [Pairing]).
 *
 * Both phones run this same screen, held FACE TO FACE: each shows its QR code and reads the other's
 * with its front camera, so one gesture exchanges both keys. When a phone has read its peer's key
 * it shows the six-digit code the two must agree on; when it also sees that the peer has read its
 * key, it closes by itself.
 *
 * The same screen hands out a team's key ([TeamInvite]). The leader opens it to invite
 * ([EXTRA_INVITE]), the new member to join ([EXTRA_JOIN]); they pair exactly as above, then the
 * leader's code becomes the team key sealed to that pairing, and the member's an acknowledgement.
 * A pairing made for a team is used for nothing else, and is not kept.
 */
class PairActivity : AppCompatActivity() {

    private val pairing = Pairing.start()
    private lateinit var qr: ImageView
    private lateinit var status: TextView
    private lateinit var code: TextView
    private lateinit var done: MaterialButton

    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null
    private val qrReader = QRCodeReader()
    private var lastDecodeMs = 0L
    private var luma = ByteArray(0)
    @Volatile private var finishing = false

    /** The team being handed out, on the leader's phone. */
    private val inviting: Team? by lazy { Teams.store(this).find(intent.getStringExtra(EXTRA_INVITE)) }
    private val joining by lazy { intent.getBooleanExtra(EXTRA_JOIN, false) }
    private val forTeam get() = inviting != null || joining
    private var key: PairingKey? = null
    private var showingInvite = false
    /** Set once the team has changed hands: the member's copy saved, or the leader shown its ack. */
    private var teamDone = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        androidx.core.view.WindowCompat.setDecorFitsSystemWindows(window, false)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // Full brightness, so the other phone's camera reads this code from across the gap.
        window.attributes = window.attributes.apply { screenBrightness = 1f }

        qr = ImageView(this).apply {
            setBackgroundColor(Color.WHITE)
            val p = dp(12)
            setPadding(p, p, p, p)
            scaleType = ImageView.ScaleType.FIT_CENTER
            background = GradientDrawable().apply {
                setColor(Color.WHITE)
                cornerRadius = dp(20).toFloat()
            }
            contentDescription = "This phone's pairing code"
        }
        status = text("Hold the two phones face to face, screens together, about 15 cm apart.").apply {
            gravity = Gravity.CENTER
        }
        code = title("", 40f).apply {
            gravity = Gravity.CENTER
            visibility = View.GONE
        }
        done = button("Done", R.drawable.ic_check_circle) { complete() }.apply {
            visibility = View.GONE
        }

        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), dp(20))
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(MaterialButton(
                    context, null, com.google.android.material.R.attr.materialIconButtonStyle,
                ).apply {
                    setIconResource(R.drawable.ic_close)
                    contentDescription = "Cancel"
                    setOnClickListener { finish() }
                })
                val heading = inviting?.let { "Invite to ${it.name}" } ?: if (joining) "Join a team" else "Pair phones"
                addView(title(heading, 24f), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
                addView(if (forTeam) helpButton("Teams", TEAM_HELP) else helpButton("Pairing", PAIR_HELP))
            })
            addView(space(12))
            addView(qr, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
            addView(space(16))
            addView(status)
            addView(code, matchWidth(dp(8)))
            addView(done, matchWidth(dp(16)))
            addView(button("Cancel", style = Style.Text) { finish() }, matchWidth(dp(4)))
        }
        setContentView(col.padForSystemBars())
        if (joining) status.text = "Hold this phone face to face with the team leader's, which shows Invite."
        if (inviting == null && intent.hasExtra(EXTRA_INVITE)) {
            status.text = "That team is no longer on this phone."
            return
        }
        showQr()
    }

    override fun onResume() {
        super.onResume()
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            openCamera()
        } else {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.CAMERA), REQ_CAMERA)
        }
    }

    override fun onPause() {
        super.onPause()
        closeCamera()
    }

    override fun onRequestPermissionsResult(code: Int, permissions: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(code, permissions, results)
        if (code == REQ_CAMERA && results.firstOrNull() == PackageManager.PERMISSION_GRANTED) {
            openCamera()
        } else {
            status.text = "Pairing needs the camera, to read the other phone's code."
        }
    }

    /**
     * Draws this phone's current code: its key, plus an acknowledgement once it has the peer's --
     * or, for a team, [text].
     */
    private fun showQr(text: String = pairing.qrText()) {
        val size = 720
        val m = QRCodeWriter().encode(
            text, BarcodeFormat.QR_CODE, size, size,
            mapOf(EncodeHintType.ERROR_CORRECTION to ErrorCorrectionLevel.M, EncodeHintType.MARGIN to 2),
        )
        val px = IntArray(size * size) { i -> if (m.get(i % size, i / size)) Color.BLACK else Color.WHITE }
        qr.setImageBitmap(Bitmap.createBitmap(px, size, size, Bitmap.Config.ARGB_8888))
    }

    private fun onRead(text: String) {
        if (finishing || teamDone) return
        if (forTeam && onTeamRead(text)) return
        when (pairing.read(text)) {
            Pairing.Read.NotOurs, Pairing.Read.Own -> return
            Pairing.Read.PeerKey -> {
                // We hold their key; our code now says so, which is what completes THEIR side.
                if (!showingInvite) showQr()
                showCode()
                status.text = "Got it. Keep holding until both phones show this code."
            }
            Pairing.Read.PeerComplete -> {
                showCode()
                val team = inviting
                val k = key
                if (team != null && k != null) {
                    // Both phones hold the pairing key, so the member can open the team's now.
                    if (!showingInvite) {
                        showingInvite = true
                        showQr(TeamInvite.encode(team, k))
                    }
                    status.text = "Paired. Handing over ${team.name}: keep holding…"
                    return
                }
                showQr()
                if (joining) {
                    status.text = "Paired. Waiting for the team…"
                    return
                }
                status.text = "Paired. Check both phones show the same code."
                // A moment longer on screen, so the other phone sees this code's acknowledgement too.
                qr.postDelayed({ complete() }, AUTO_CLOSE_MS)
            }
        }
    }

    /** A team invite or acknowledgement in [text]: true when it was one, handled. */
    private fun onTeamRead(text: String): Boolean {
        val isTeamCode = text.startsWith(TeamInvite.PREFIX) || text.startsWith(TeamInvite.ACK_PREFIX)
        val k = key ?: return isTeamCode
        val team = inviting
        if (team != null && TeamInvite.isAck(text, team, k)) {
            teamDone = true
            status.text = "Done: the other phone has joined ${team.name}."
            buzz()
            qr.postDelayed({ complete() }, AUTO_CLOSE_MS)
            return true
        }
        if (joining && text.startsWith(TeamInvite.PREFIX)) {
            val joined = TeamInvite.decode(text, k) ?: run {
                status.text = "That team code is for a different pairing. Cancel and pair with the leader again."
                return true
            }
            Teams.store(this).add(joined)
            teamDone = true
            // Shown until the leader reads it, which is how THEIR screen knows to say done.
            showQr(TeamInvite.ack(joined, k))
            status.text = "Joined ${joined.name}. Hold a moment longer so the other phone sees it."
            buzz()
            qr.postDelayed({ complete() }, AUTO_CLOSE_MS * 2)
            return true
        }
        return isTeamCode
    }

    private fun showCode() {
        val k = key ?: pairing.key() ?: return
        // A pairing made to pass on a team is not the phone's pairing: that is left as it was.
        if (!forTeam) PairingStore.set(k)
        if (key == null) buzz()
        key = k
        code.text = k.code
        code.visibility = View.VISIBLE
        done.visibility = View.VISIBLE
    }

    private fun buzz() {
        window.decorView.performHapticFeedback(android.view.HapticFeedbackConstants.CONFIRM)
    }

    private fun complete() {
        if (forTeam) {
            if (finishing) return
            finishing = true
            if (teamDone) setResult(RESULT_OK)
            finish()
            return
        }
        if (finishing || PairingStore.current() == null) return
        finishing = true
        setResult(RESULT_OK)
        finish()
    }

    // ---------------------------------------------------------------------------------- camera

    private fun openCamera() {
        if (camera != null) return
        val mgr = getSystemService(CAMERA_SERVICE) as CameraManager
        val id = mgr.cameraIdList.firstOrNull {
            mgr.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) ==
                CameraCharacteristics.LENS_FACING_FRONT
        } ?: run {
            status.text = "This phone has no front camera, so it can't pair."
            return
        }
        val chars = mgr.getCameraCharacteristics(id)
        // About 720p: plenty for a QR code a hand-width away, and cheap to scan several times a second.
        val size = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?.getOutputSizes(ImageFormat.YUV_420_888)
            ?.filter { it.width * it.height <= 1920 * 1080 }
            ?.minByOrNull { kotlin.math.abs(it.width * it.height - 1280 * 720) }
            ?: return
        val t = HandlerThread("ff-pair-camera").apply { start() }
        thread = t
        val h = Handler(t.looper)
        handler = h
        val r = ImageReader.newInstance(size.width, size.height, ImageFormat.YUV_420_888, 2)
        reader = r
        r.setOnImageAvailableListener({ ir ->
            val image = ir.acquireLatestImage() ?: return@setOnImageAvailableListener
            try {
                val now = SystemClock.elapsedRealtime()
                if (now - lastDecodeMs < DECODE_EVERY_MS) return@setOnImageAvailableListener
                lastDecodeMs = now
                val plane = image.planes[0]
                val w = image.width
                val ht = image.height
                if (luma.size != w * ht) luma = ByteArray(w * ht)
                val buf = plane.buffer
                for (row in 0 until ht) {
                    buf.position(row * plane.rowStride)
                    buf.get(luma, row * w, w)
                }
                decode(luma, w, ht)?.let { text -> runOnUiThread { onRead(text) } }
            } catch (_: Exception) {
                // One bad frame: the next is a fifth of a second away.
            } finally {
                image.close()
            }
        }, h)

        try {
            mgr.openCamera(id, java.util.concurrent.Executor { h.post(it) }, object : CameraDevice.StateCallback() {
                override fun onOpened(device: CameraDevice) {
                    camera = device
                    val request = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                        addTarget(r.surface)
                        val af = chars.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES)
                        if (af?.contains(CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE) == true) {
                            set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                        }
                    }.build()
                    val config = SessionConfiguration(
                        SessionConfiguration.SESSION_REGULAR,
                        listOf(OutputConfiguration(r.surface)),
                        java.util.concurrent.Executor { h.post(it) },
                        object : CameraCaptureSession.StateCallback() {
                            override fun onConfigured(s: CameraCaptureSession) {
                                session = s
                                runCatching { s.setRepeatingRequest(request, null, h) }
                            }

                            override fun onConfigureFailed(s: CameraCaptureSession) {
                                runOnUiThread { status.text = "The front camera couldn't start." }
                            }
                        },
                    )
                    runCatching { device.createCaptureSession(config) }
                }

                override fun onDisconnected(device: CameraDevice) {
                    device.close()
                    camera = null
                }

                override fun onError(device: CameraDevice, error: Int) {
                    device.close()
                    camera = null
                    runOnUiThread { status.text = "The front camera couldn't start." }
                }
            })
        } catch (e: SecurityException) {
            status.text = "Pairing needs the camera, to read the other phone's code."
        }
    }

    private fun decode(data: ByteArray, w: Int, h: Int): String? {
        val bitmap = BinaryBitmap(HybridBinarizer(PlanarYUVLuminanceSource(data, w, h, 0, 0, w, h, false)))
        return try {
            qrReader.decode(bitmap, mapOf(DecodeHintType.TRY_HARDER to true)).text
        } catch (_: ReaderException) {
            null
        } finally {
            qrReader.reset()
        }
    }

    private fun closeCamera() {
        runCatching { session?.close() }
        runCatching { camera?.close() }
        runCatching { reader?.close() }
        session = null
        camera = null
        reader = null
        thread?.quitSafely()
        thread = null
        handler = null
    }

    companion object {
        /** The [Team.idHex] of a team to hand out. */
        const val EXTRA_INVITE = "invite"
        /** Join a team, from a leader's phone showing [EXTRA_INVITE]. */
        const val EXTRA_JOIN = "join"
        private const val REQ_CAMERA = 7
        private const val DECODE_EVERY_MS = 150L
        private const val AUTO_CLOSE_MS = 1800L
        private const val PAIR_HELP =
            "Encrypted mode seals each file so that only the paired phone can open it.\n\n" +
                "To pair, open this screen on both phones and hold them face to face. Each reads " +
                "the other's code with its front camera — nothing goes over a network.\n\n" +
                "Both phones then show a six-digit code. If they match, the pairing is yours. If " +
                "they don't, cancel and pair again.\n\n" +
                "A pairing lasts until you close FileFlow, or 30 minutes."
        private const val TEAM_HELP =
            "The leader taps Invite on the team; the new member taps Join a team. Hold the two " +
                "phones face to face, as for pairing.\n\n" +
                "They pair first, and check the six-digit codes match. Then the leader's screen " +
                "shows the team's key, sealed so that only the phone it just paired with can open " +
                "it — anyone else's camera learns nothing from it.\n\n" +
                "When the member's phone has the key, both screens say so."
    }
}
