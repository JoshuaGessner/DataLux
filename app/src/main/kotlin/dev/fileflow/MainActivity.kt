package dev.fileflow

import android.content.ContentUris
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.provider.MediaStore
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.edit
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import com.google.android.material.bottomnavigation.BottomNavigationView
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.materialswitch.MaterialSwitch
import dev.fileflow.rx.LiveDecoder
import dev.fileflow.secure.PairingStore
import dev.fileflow.ui.Ui.Style
import dev.fileflow.ui.Ui.button
import dev.fileflow.ui.Ui.card
import dev.fileflow.ui.Ui.dp
import dev.fileflow.ui.Ui.helpButton
import dev.fileflow.ui.Ui.matchWidth
import dev.fileflow.ui.Ui.showHelp
import dev.fileflow.ui.Ui.space
import dev.fileflow.ui.Ui.text
import dev.fileflow.ui.Ui.themeColor
import dev.fileflow.ui.Ui.title

/**
 * The app's front door: four tabs, each one screen with no scrolling.
 *
 *  - **Send** and **Receive** -- the two things a person does, each a single action on one screen.
 *  - **Received** -- what has arrived, to open again.
 *  - **Settings** -- the speed choice, how it works, the device check; and the place anything
 *    added later goes, so the operational screens stay one tap deep.
 *
 * The previous version stacked everything on one scrolling page, and the button someone came for
 * was below the fold.
 *
 * Everything remains scriptable: the activities take the same intent extras, and this screen only
 * supplies defaults. The grid is the sender's choice alone (the speed setting); the receiver
 * detects it (F58), and the transmitter picks its pixel pitch from the surface it is given (F31).
 */
class MainActivity : AppCompatActivity() {

    private var speed = Speed.DEFAULT
    private lateinit var nav: BottomNavigationView
    private lateinit var speedLabel: MaterialButton
    private val pages = HashMap<Int, View>()
    private var receivedList: LinearLayout? = null

    private val pickFile = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.OpenDocument(),
    ) { uri -> if (uri != null) send(uri) }

    /** Encrypted mode: files are sealed to a paired phone (`secure/`). One switch, both tabs. */
    private var encrypted = false
    private val securityRows = ArrayList<Pair<MaterialSwitch, TextView>>()
    private var afterPairing: (() -> Unit)? = null
    private val pair = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.StartActivityForResult(),
    ) { r ->
        refreshSecurity()
        val next = afterPairing
        afterPairing = null
        if (r.resultCode == RESULT_OK) next?.invoke()
    }

    /** Runs [action] now, or after pairing when encrypted mode is on and the phones are not paired. */
    private fun withPairing(action: () -> Unit) {
        if (!encrypted || PairingStore.current() != null) {
            action()
        } else {
            afterPairing = action
            pair.launch(Intent(this, PairActivity::class.java))
        }
    }

    /** Opens the transmitter on [uri], or on the generated test file when null. */
    private fun send(uri: android.net.Uri?) = withPairing {
        val i = Intent(this, TransmitActivity::class.java)
            .putExtra("cols", speed.cols)
            .putExtra("rows", speed.rows)
            .putExtra("stateRate", speed.statesPerSecond)
            .putExtra("colour", speed.colour)
            .putExtra("encrypted", encrypted)
        if (uri != null) {
            // The picker's grant covers this activity; the flag passes it on to the transmitter.
            i.setData(uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(i)
    }

    private fun receive() = withPairing {
        // LIVE: decode on the phone until the file verifies (C13b). The receiver chooses its own
        // capture rate, exposure and grid; the sender's grid is only tried first. Scripted
        // experiments still reach the recording path by launching CaptureActivity without `live`.
        // 4096 wide: resolution sets how far away the phone can be held (F57).
        startActivity(
            Intent(this, CaptureActivity::class.java)
                .putExtra("gridCols", speed.cols)
                .putExtra("gridRows", speed.rows)
                .putExtra("live", true)
                .putExtra("maxWidth", 4096)
                .putExtra("encrypted", encrypted)
        )
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val prefs = getPreferences(MODE_PRIVATE)
        speed = Speed.named(prefs.getString(PREF_SPEED, null))
        encrypted = prefs.getBoolean(PREF_ENCRYPTED, false)

        val content = FrameLayout(this)
        pages[TAB_SEND] = sendPage()
        pages[TAB_RECEIVE] = receivePage()
        pages[TAB_RECEIVED] = receivedPage()
        pages[TAB_SETTINGS] = settingsPage(prefs)
        pages.values.forEach { content.addView(it, FrameLayout.LayoutParams(MATCH, MATCH)) }

        nav = BottomNavigationView(this).apply {
            menu.add(0, TAB_SEND, 0, "Send").setIcon(R.drawable.ic_upload)
            menu.add(0, TAB_RECEIVE, 1, "Receive").setIcon(R.drawable.ic_camera)
            menu.add(0, TAB_RECEIVED, 2, "Received").setIcon(R.drawable.ic_folder)
            menu.add(0, TAB_SETTINGS, 3, "Settings").setIcon(R.drawable.ic_settings)
            labelVisibilityMode = com.google.android.material.navigation.NavigationBarView
                .LABEL_VISIBILITY_LABELED
            setOnItemSelectedListener { item ->
                show(item.itemId)
                prefs.edit { putInt(PREF_TAB, item.itemId) }
                true
            }
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(themeColor(android.R.attr.colorBackground))
            addView(content, LinearLayout.LayoutParams(MATCH, 0, 1f))
            addView(nav, LinearLayout.LayoutParams(MATCH, WRAP))
        }
        // Top and sides only: the navigation bar pads itself for the gesture area.
        ViewCompat.setOnApplyWindowInsetsListener(root) { v, insets ->
            val i = insets.getInsets(SYSTEM_INSETS)
            v.setPadding(i.left, i.top, i.right, 0)
            insets
        }
        setContentView(root)
        val tab = prefs.getInt(PREF_TAB, TAB_SEND).takeIf { it in pages } ?: TAB_SEND
        nav.selectedItemId = tab
        show(tab, animate = false)
        if (savedInstanceState == null) handleShortcut(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleShortcut(intent)
    }

    /** Launcher shortcuts (res/xml/shortcuts.xml): straight to receiving, or to the picker. */
    private fun handleShortcut(i: Intent?) {
        when (i?.action) {
            ACTION_RECEIVE -> receive()
            ACTION_SEND_FILE -> {
                nav.selectedItemId = TAB_SEND
                pickFile.launch(arrayOf("*/*"))
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refreshReceived()
        refreshSecurity()  // a pairing expires on its own
    }

    /**
     * The encrypted-mode switch, with the pairing's state under it. The same row sits on Send and
     * Receive, because it has to be on for BOTH phones and each person is looking at a different tab.
     */
    private fun securityRow(): View {
        val sw = MaterialSwitch(this).apply {
            isChecked = encrypted
            contentDescription = "Encrypted mode"
        }
        val sub = text("", 13f)
        securityRows += sw to sub
        sw.setOnCheckedChangeListener { _, on ->
            if (on == encrypted) return@setOnCheckedChangeListener
            encrypted = on
            getPreferences(MODE_PRIVATE).edit { putBoolean(PREF_ENCRYPTED, on) }
            if (!on) PairingStore.forget()
            refreshSecurity()
        }
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(4), dp(4), 0, dp(8))
            addView(ImageView(context).apply {
                setImageResource(R.drawable.ic_lock)
                imageTintList = ColorStateList.valueOf(
                    themeColor(com.google.android.material.R.attr.colorOnSurfaceVariant),
                )
            }, LinearLayout.LayoutParams(dp(24), dp(24)))
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(12), 0, dp(8), 0)
                addView(text("Encrypted", 16f, com.google.android.material.R.attr.colorOnSurface))
                addView(sub)
                // Tapping the state pairs (again): the way to recover from a mismatched code.
                setOnClickListener {
                    if (encrypted) pair.launch(Intent(this@MainActivity, PairActivity::class.java))
                }
            }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(sw)
            addView(helpButton("Encrypted mode", ENCRYPTION_HELP))
        }
    }

    private fun refreshSecurity() {
        val key = PairingStore.current()
        val state = when {
            !encrypted -> "Off"
            key != null -> "Paired · ${key.code}"
            else -> "Not paired · tap to pair"
        }
        securityRows.forEach { (sw, sub) ->
            if (sw.isChecked != encrypted) sw.isChecked = encrypted
            sub.text = state
        }
    }

    private fun show(tab: Int, animate: Boolean = true) {
        pages.forEach { (id, page) ->
            if (id == tab) {
                if (page.visibility != View.VISIBLE && animate) {
                    page.alpha = 0f
                    page.translationY = dp(8).toFloat()
                    page.animate().alpha(1f).translationY(0f).setDuration(160L).start()
                }
                page.visibility = View.VISIBLE
            } else {
                page.visibility = View.GONE
            }
        }
        if (tab == TAB_RECEIVED) refreshReceived()
    }

    // ------------------------------------------------------------------------------------ pages

    /** A page title and its "?", the same on every tab. */
    private fun header(name: String, helpTitle: String? = null, help: String? = null) =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(20), dp(12), dp(8), dp(4))
            addView(title(name, 28f), LinearLayout.LayoutParams(0, WRAP, 1f))
            if (help != null) addView(helpButton(helpTitle ?: name, help))
        }

    /** A large icon in a soft circle: the one picture on an action page. */
    private fun hero(icon: Int) = ImageView(this).apply {
        setImageResource(icon)
        imageTintList = ColorStateList.valueOf(
            themeColor(com.google.android.material.R.attr.colorOnPrimaryContainer),
        )
        background = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(themeColor(com.google.android.material.R.attr.colorPrimaryContainer))
        }
        val p = dp(30)
        setPadding(p, p, p, p)
        layoutParams = LinearLayout.LayoutParams(dp(120), dp(120)).apply {
            gravity = Gravity.CENTER_HORIZONTAL
        }
    }

    /** Header at the top, the content centred in what is left, actions at the bottom. */
    private fun actionPage(header: View, middle: List<View>, actions: List<View>) =
        LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(header)
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER
                setPadding(dp(28), 0, dp(28), 0)
                middle.forEach { addView(it) }
            }, LinearLayout.LayoutParams(MATCH, 0, 1f))
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(20), 0, dp(20), dp(16))
                actions.forEach { addView(it) }
            })
        }

    private fun centred(v: android.widget.TextView) = v.apply { gravity = Gravity.CENTER }

    private fun sendPage(): View {
        speedLabel = button("", R.drawable.ic_tune, Style.Text) {
            nav.selectedItemId = TAB_SETTINGS
        }.apply { layoutParams = matchWidth(dp(4)) }
        updateSpeedLabel()
        return actionPage(
            header("Send", "How it works", HOW_IT_WORKS),
            listOf(
                hero(R.drawable.ic_upload),
                space(20),
                centred(title("Send a file", 22f)),
                space(6),
                centred(text("It appears on this screen as light for the other phone to scan.")),
            ),
            listOf(
                securityRow(),
                button("Choose a file", R.drawable.ic_file) {
                    withPairing { pickFile.launch(arrayOf("*/*")) }
                }.apply { layoutParams = matchWidth() },
                button("Send a test file", R.drawable.ic_bolt, Style.Tonal) { send(null) }
                    .apply { layoutParams = matchWidth(dp(10)) },
                speedLabel,
            ),
        )
    }

    private fun updateSpeedLabel() {
        if (::speedLabel.isInitialized) speedLabel.text = "Speed: ${speed.label}"
    }

    private fun receivePage(): View = actionPage(
        header("Receive", "Tips", RECEIVE_HELP),
        listOf(
            hero(R.drawable.ic_camera),
            space(20),
            centred(title("Receive a file", 22f)),
            space(6),
            centred(text("Fill this camera's picture with the sending screen.")),
        ),
        listOf(
            securityRow(),
            button("Start receiving", R.drawable.ic_camera) { receive() }
                .apply { layoutParams = matchWidth() },
        ),
    )

    private fun receivedPage(): View {
        val list = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(4), dp(16), dp(16))
        }
        receivedList = list
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(header("Received", "Received files",
                "Verified files are saved to Downloads › ${LiveDecoder.DOWNLOADS_SUBDIR}. " +
                    "Tap one to open it, or use your Files app."))
            // A list may scroll -- it is the one thing here that grows.
            addView(ScrollView(context).apply { addView(list) },
                LinearLayout.LayoutParams(MATCH, 0, 1f))
        }
    }

    /** Files this app saved to Downloads/FileFlow, newest first. */
    private fun refreshReceived() {
        val list = receivedList ?: return
        list.removeAllViews()
        data class Item(
            val uri: android.net.Uri, val name: String, val bytes: Long, val mime: String?,
            val addedS: Long,
        )
        val items = ArrayList<Item>()
        runCatching {
            contentResolver.query(
                MediaStore.Downloads.EXTERNAL_CONTENT_URI,
                arrayOf(MediaStore.MediaColumns._ID, MediaStore.MediaColumns.DISPLAY_NAME,
                        MediaStore.MediaColumns.SIZE, MediaStore.MediaColumns.MIME_TYPE,
                        MediaStore.MediaColumns.DATE_ADDED),
                "${MediaStore.MediaColumns.RELATIVE_PATH} LIKE ?",
                arrayOf("%/${LiveDecoder.DOWNLOADS_SUBDIR}/%"),
                "${MediaStore.MediaColumns.DATE_ADDED} DESC",
            )?.use { c ->
                while (c.moveToNext() && items.size < 100) {
                    items += Item(
                        ContentUris.withAppendedId(MediaStore.Downloads.EXTERNAL_CONTENT_URI, c.getLong(0)),
                        c.getString(1) ?: "file", c.getLong(2), c.getString(3), c.getLong(4),
                    )
                }
            }
        }
        if (items.isEmpty()) {
            list.addView(centred(text("Nothing received yet.")).apply { setPadding(0, dp(48), 0, 0) },
                matchWidth())
            return
        }
        items.forEach { item ->
            val mime = item.mime ?: "application/octet-stream"
            val whenText = android.text.format.DateUtils.getRelativeTimeSpanString(
                item.addedS * 1000, System.currentTimeMillis(),
                android.text.format.DateUtils.MINUTE_IN_MILLIS,
            )
            list.addView(row(
                R.drawable.ic_file, item.name, "${TransmitActivity.formatBytes(item.bytes)} · $whenText",
                onLongClick = { anchor -> fileMenu(anchor, item.uri, item.name, mime) },
                thumbnailOf = if (mime.startsWith("image/") || mime.startsWith("video/")) item.uri else null,
            ) {
                val view = Intent(Intent.ACTION_VIEW)
                    .setDataAndType(item.uri, mime)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                runCatching { startActivity(Intent.createChooser(view, item.name)) }
            }, matchWidth(dp(8)))
        }
        list.addView(centred(text("Long-press a file to share or delete it.", 13f))
            .apply { setPadding(0, dp(12), 0, 0) }, matchWidth())
    }

    /** Share or delete a received file. */
    private fun fileMenu(anchor: View, uri: android.net.Uri, name: String, mime: String) {
        android.widget.PopupMenu(this, anchor).apply {
            menu.add("Share").setOnMenuItemClickListener {
                val send = Intent(Intent.ACTION_SEND)
                    .setType(mime)
                    .putExtra(Intent.EXTRA_STREAM, uri)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                runCatching { startActivity(Intent.createChooser(send, name)) }
                true
            }
            menu.add("Delete").setOnMenuItemClickListener {
                com.google.android.material.dialog.MaterialAlertDialogBuilder(this@MainActivity)
                    .setTitle("Delete $name?")
                    .setMessage("It will be removed from Downloads › ${LiveDecoder.DOWNLOADS_SUBDIR}.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Delete") { _, _ ->
                        // Files this app saved are its own to delete. One saved by an earlier
                        // install is not, and Android would need the person's consent; the Files
                        // app is the honest route for that rare case.
                        val ok = runCatching { contentResolver.delete(uri, null, null) > 0 }
                            .getOrDefault(false)
                        if (!ok) {
                            android.widget.Toast.makeText(
                                this@MainActivity, "Delete it from the Files app.",
                                android.widget.Toast.LENGTH_LONG,
                            ).show()
                        }
                        refreshReceived()
                    }
                    .show()
                true
            }
            show()
        }
    }

    private fun settingsPage(prefs: android.content.SharedPreferences): View {
        val toggle = MaterialButtonToggleGroup(this).apply {
            isSingleSelection = true
            isSelectionRequired = true
            Speed.entries.forEach { s ->
                // Outlined style, untouched: inside a toggle group it is what draws the checked
                // segment filled and the others clear.
                addView(MaterialButton(
                    context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle,
                ).apply {
                    id = View.generateViewId()
                    tag = s
                    text = s.label
                    isAllCaps = false
                    textSize = 14f
                    minHeight = dp(48)
                    maxLines = 1
                    // The default 24 dp side padding truncated "Steady" to "Ste…".
                    setPaddingRelative(dp(4), paddingTop, dp(4), paddingBottom)
                }, LinearLayout.LayoutParams(0, WRAP, 1f))
            }
            check((0 until childCount).map { getChildAt(it) }.first { it.tag == speed }.id)
            addOnButtonCheckedListener { group, id, checked ->
                if (!checked) return@addOnButtonCheckedListener
                speed = group.findViewById<View>(id).tag as Speed
                prefs.edit { putString(PREF_SPEED, speed.name) }
                updateSpeedLabel()
            }
        }
        val body = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(4), dp(16), dp(16))
            addView(card(
                LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    addView(title("Sending speed", 18f), LinearLayout.LayoutParams(0, WRAP, 1f))
                    addView(helpButton("Sending speed", SPEED_HELP))
                },
                space(8),
                toggle.apply { layoutParams = matchWidth() },
            ), matchWidth())
            addView(row(R.drawable.ic_help, "How it works", null) {
                showHelp("How it works", HOW_IT_WORKS)
            }, matchWidth(dp(12)))
            addView(row(R.drawable.ic_tune, "Device check", "What this phone's camera and screen can do") {
                startActivity(Intent(this@MainActivity, ProbeActivity::class.java))
            }, matchWidth(dp(8)))
            addView(text("Version ${runCatching {
                packageManager.getPackageInfo(packageName, 0).versionName
            }.getOrNull() ?: "?"} · open source, AGPL-3.0", 13f).apply {
                gravity = Gravity.CENTER
                setPadding(0, dp(20), 0, 0)
            }, matchWidth())
        }
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(header("Settings"))
            addView(body)
        }
    }

    /** A tappable list row: icon, title, optional subtitle, chevron. */
    private fun row(
        icon: Int,
        title: String,
        subtitle: String?,
        onLongClick: ((View) -> Unit)? = null,
        thumbnailOf: android.net.Uri? = null,
        onClick: () -> Unit,
    ): View =
        card(LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            val iconView = ImageView(context).apply {
                setImageResource(icon)
                imageTintList = ColorStateList.valueOf(
                    themeColor(com.google.android.material.R.attr.colorOnSurfaceVariant),
                )
            }
            val side = if (thumbnailOf != null) dp(48) else dp(24)
            addView(iconView, LinearLayout.LayoutParams(side, side))
            // A picture is recognised faster than its file name. Loaded off the main thread;
            // the generic icon stays if the provider has no thumbnail to give.
            if (thumbnailOf != null) {
                iconView.scaleType = ImageView.ScaleType.CENTER_INSIDE
                Thread {
                    val bmp = runCatching {
                        contentResolver.loadThumbnail(thumbnailOf, android.util.Size(dp(96), dp(96)), null)
                    }.getOrNull() ?: return@Thread
                    iconView.post {
                        iconView.imageTintList = null
                        iconView.scaleType = ImageView.ScaleType.CENTER_CROP
                        iconView.clipToOutline = true
                        iconView.outlineProvider = object : android.view.ViewOutlineProvider() {
                            override fun getOutline(v: View, o: android.graphics.Outline) {
                                o.setRoundRect(0, 0, v.width, v.height, dp(10).toFloat())
                            }
                        }
                        iconView.setImageBitmap(bmp)
                    }
                }.start()
            }
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(16), 0, dp(8), 0)
                addView(text(title, 16f, com.google.android.material.R.attr.colorOnSurface).apply {
                    maxLines = 1
                    ellipsize = android.text.TextUtils.TruncateAt.MIDDLE
                })
                if (subtitle != null) addView(text(subtitle, 13f))
            }, LinearLayout.LayoutParams(0, WRAP, 1f))
            addView(ImageView(context).apply {
                setImageResource(R.drawable.ic_chevron)
                imageTintList = ColorStateList.valueOf(
                    themeColor(com.google.android.material.R.attr.colorOnSurfaceVariant),
                )
            }, LinearLayout.LayoutParams(dp(20), dp(20)))
        }).apply {
            radius = dp(20).toFloat()
            (getChildAt(0) as? LinearLayout)?.setPadding(dp(18), dp(16), dp(14), dp(16))
            isClickable = true
            isFocusable = true
            setOnClickListener { onClick() }
            if (onLongClick != null) setOnLongClickListener { onLongClick(it); true }
        }

    companion object {
        const val ACTION_RECEIVE = "dev.fileflow.action.RECEIVE"
        const val ACTION_SEND_FILE = "dev.fileflow.action.SEND_FILE"
        private const val PREF_SPEED = "speed"
        private const val PREF_ENCRYPTED = "encrypted"
        private const val PREF_TAB = "tab"
        private const val TAB_SEND = 1
        private const val TAB_RECEIVE = 2
        private const val TAB_RECEIVED = 3
        private const val TAB_SETTINGS = 4
        private const val MATCH = LinearLayout.LayoutParams.MATCH_PARENT
        private const val WRAP = LinearLayout.LayoutParams.WRAP_CONTENT

        private const val HOW_IT_WORKS =
            "One phone shows the file as a fast-changing pattern of light. The other phone's " +
                "camera reads it.\n\n" +
                "Nothing goes over Wi-Fi, Bluetooth, NFC or a cable. Every file is checked " +
                "(SHA-256) before it is saved, so a received file is exactly the one sent.\n\n" +
                "Received files go to Downloads › FileFlow."

        private const val RECEIVE_HELP =
            "• Fill the picture with the sending screen: its pattern just inside the corner " +
                "marks. That is where it reads fastest.\n\n" +
                "• Keep the two phones parallel.\n\n" +
                "• It finds the sender's speed setting by itself.\n\n" +
                "• If it pauses, just aim back: nothing received so far is lost."

        private const val ENCRYPTION_HELP =
            "Off: like a QR code, anyone whose camera can see the sending screen could read the " +
                "file.\n\n" +
                "On: each file is sealed with AES-256 to a key only the two paired phones hold. " +
                "Pair by holding the phones face to face — each reads the other's code with its " +
                "front camera. Nothing goes over a network, and the key is never stored: it lasts " +
                "until you close FileFlow, or 30 minutes.\n\n" +
                "Turn it on on BOTH phones. In encrypted mode the receiver refuses any file that " +
                "isn't sealed to its pairing."

        private const val SPEED_HELP =
            "Fast — eight colours, three times the data of black and white, at 20 frames a " +
                "second.\n\n" +
                "Fastest — colour at 30 frames a second, for a receiver whose camera runs at 50–60 " +
                "fps (most recent phones).\n\n" +
                "Steady — black and white. Slower, but for a camera or lighting colour doesn't " +
                "survive.\n\n" +
                "Only the sending phone needs this setting; the receiver works it out."
    }
}
