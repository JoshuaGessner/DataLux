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
import dev.fileflow.secure.Envelope
import dev.fileflow.secure.PairingStore
import dev.fileflow.secure.Team
import dev.fileflow.secure.Teams
import dev.fileflow.ui.HeroView
import dev.fileflow.ui.LightGridView
import dev.fileflow.ui.Motion
import dev.fileflow.ui.ScanView
import dev.fileflow.ui.Ui.Style
import dev.fileflow.ui.Ui.capped
import dev.fileflow.ui.Ui.emptyState
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
    private lateinit var nav: com.google.android.material.navigation.NavigationBarView
    private lateinit var speedLabel: MaterialButton
    private val pages = HashMap<Int, View>()
    /** Per tab, what rises into place when it is shown, in reading order ([Motion.staggerIn]). */
    private val entrance = HashMap<Int, () -> List<View>>()
    private var receivedList: LinearLayout? = null

    private val pickFile = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.OpenMultipleDocuments(),
    ) { uris -> if (uris.isNotEmpty()) send(uris) }

    /** Encrypted mode: files are sealed to a paired phone (`secure/`). One switch, both tabs. */
    private var encrypted = false
    /** The team files are sealed to when sending ([Team.idHex]); null for the paired phone. */
    private var sealTo: String? = null
    /** How hard the sender compresses: the Settings choice, Auto unless changed. */
    private var compression = Envelope.Mode.Auto
    private val securityRows = ArrayList<Pair<MaterialSwitch, TextView>>()
    /** Each row's lock, closed and lit when encrypted mode is on. */
    private val locks = ArrayList<ImageView>()
    private var afterPairing: (() -> Unit)? = null
    private val pair = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.StartActivityForResult(),
    ) { r ->
        refreshSecurity()
        val next = afterPairing
        afterPairing = null
        if (r.resultCode == RESULT_OK) next?.invoke()
    }

    /** The team to send to, when one is chosen, still on this phone, and part of this plan. */
    private fun sealTeam(): Team? =
        if (Entitlements.has(Entitlements.Feature.Teams)) Teams.store(this).find(sealTo) else null

    /**
     * Runs [action] now, or after pairing when encrypted mode is on and there is no key to use:
     * no pairing, and -- sending -- no team chosen or -- [receiving] -- no team at all.
     */
    private fun withPairing(receiving: Boolean = false, action: () -> Unit) {
        val teamKey = if (receiving) {
            Entitlements.has(Entitlements.Feature.Teams) && Teams.store(this).all().isNotEmpty()
        } else {
            sealTeam() != null
        }
        if (!encrypted || PairingStore.current() != null || teamKey) {
            action()
        } else {
            afterPairing = action
            pair.launch(Intent(this, PairActivity::class.java))
        }
    }

    /** Opens the transmitter on [uris] -- several are sent as one batch -- or the test file if none. */
    private fun send(uris: List<android.net.Uri>) = withPairing {
        if (uris.size > 1 && !Entitlements.check(this, Entitlements.Feature.Batch)) return@withPairing
        val i = Intent(this, TransmitActivity::class.java)
            .putExtra("cols", speed.cols)
            .putExtra("rows", speed.rows)
            .putExtra("stateRate", speed.statesPerSecond)
            .putExtra("colours", speed.colours)
            .putExtra("encrypted", encrypted)
            .putExtra("team", if (encrypted) sealTeam()?.idHex else null)
            .putExtra("compression", compression.name)
        if (uris.isNotEmpty()) {
            // The picker's grant covers this activity; the flag passes it on to the transmitter,
            // for the data URI and every URI in the clip data.
            i.setData(uris[0]).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            if (uris.size > 1) {
                val clip = android.content.ClipData.newRawUri("files", uris[0])
                uris.drop(1).forEach { clip.addItem(android.content.ClipData.Item(it)) }
                i.clipData = clip
            }
        }
        startActivity(i)
    }

    private fun receive() = withPairing(receiving = true) {
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
        sealTo = prefs.getString(PREF_SEAL_TO, null)
        compression = runCatching {
            Envelope.Mode.valueOf(prefs.getString(PREF_COMPRESSION, null) ?: "")
        }.getOrDefault(Envelope.Mode.Auto)

        val content = FrameLayout(this)
        pages[TAB_SEND] = sendPage()
        pages[TAB_RECEIVE] = receivePage()
        pages[TAB_RECEIVED] = receivedPage()
        pages[TAB_SETTINGS] = settingsPage(prefs)
        // Each page held to a readable width: on a tablet, or a phone on its side, a column of
        // text and buttons stretched edge to edge is hard to read and far to reach.
        pages.keys.toList().forEach { pages[it] = capped(pages.getValue(it), if (twoPane && (it == TAB_SEND || it == TAB_RECEIVE)) 1040 else 600) }
        pages.values.forEach { content.addView(it, FrameLayout.LayoutParams(MATCH, MATCH)) }

        // A tablet gets a navigation rail down the side, as Material lays out a large screen; a
        // phone the bar along the bottom, within reach of a thumb.
        nav = (if (tablet) com.google.android.material.navigationrail.NavigationRailView(this)
               else BottomNavigationView(this)).apply {
            menu.add(0, TAB_SEND, 0, "Send").setIcon(R.drawable.ic_upload)
            menu.add(0, TAB_RECEIVE, 1, "Receive").setIcon(R.drawable.ic_camera)
            menu.add(0, TAB_RECEIVED, 2, "Received").setIcon(R.drawable.ic_folder)
            menu.add(0, TAB_SETTINGS, 3, "Settings").setIcon(R.drawable.ic_settings)
            labelVisibilityMode = com.google.android.material.navigation.NavigationBarView
                .LABEL_VISIBILITY_LABELED
            // On a tablet, the rail's destinations sit in the middle of its height, in easy reach.
            (this as? com.google.android.material.navigationrail.NavigationRailView)?.menuGravity = Gravity.CENTER
            setOnItemSelectedListener { item ->
                show(item.itemId)
                prefs.edit { putInt(PREF_TAB, item.itemId) }
                true
            }
        }

        val root = LinearLayout(this).apply {
            setBackgroundColor(themeColor(android.R.attr.colorBackground))
            if (tablet) {
                orientation = LinearLayout.HORIZONTAL
                addView(nav, LinearLayout.LayoutParams(WRAP, MATCH))
                addView(content, LinearLayout.LayoutParams(0, MATCH, 1f))
            } else {
                orientation = LinearLayout.VERTICAL
                addView(content, LinearLayout.LayoutParams(MATCH, 0, 1f))
                addView(nav, LinearLayout.LayoutParams(MATCH, WRAP))
            }
        }
        // Nothing on this screen restores itself: what matters (the tab, the settings) is saved in
        // preferences as it changes. Restoring would crash: the speed buttons' ids are generated,
        // so a rebuilt screen -- folded, resized, its theme changed -- can hand a button's saved
        // state to a view of another kind.
        root.isSaveFromParentEnabled = false
        // Top and sides only: the navigation bar pads itself for the gesture area.
        ViewCompat.setOnApplyWindowInsetsListener(root) { v, insets ->
            val i = insets.getInsets(SYSTEM_INSETS)
            v.setPadding(i.left, i.top, i.right, 0)
            insets
        }
        setContentView(root)
        onBackPressedDispatcher.addCallback(this, clearSelectionOnBack)
        val tab = prefs.getInt(PREF_TAB, TAB_SEND).takeIf { it in pages } ?: TAB_SEND
        nav.selectedItemId = tab
        show(tab, animate = false)
        // Opening the app, the first page's contents rise into place too.
        entrance[tab]?.invoke()?.let { Motion.staggerIn(it, risePx = dp(20).toFloat()) }
        if (savedInstanceState == null) handleShortcut(intent)
    }

    /**
     * A tablet turned on its side: the portrait lock doesn't hold on a large screen, and the page
     * is laid out for its shape -- side by side, or stacked -- so it is built again. The tab is
     * kept (it is saved as it changes); a phone never gets here.
     */
    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        if (tablet) recreate()
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
            ACTION_SHOW_RECEIVED -> nav.selectedItemId = TAB_RECEIVED
        }
    }

    override fun onResume() {
        super.onResume()
        ProAccount.refreshInBackground()
        if (!Entitlements.has(Entitlements.Feature.BulkManage)) clearSelection()
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
        val lock = ImageView(this)
        locks += lock
        setLock(lock, encrypted, animate = false)
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            // A pill of its own: a control, set apart from the buttons below it.
            setPadding(dp(16), dp(8), dp(4), dp(8))
            background = GradientDrawable().apply {
                cornerRadius = dp(28).toFloat()
                setColor(themeColor(com.google.android.material.R.attr.colorSurfaceContainer))
            }
            layoutParams = matchWidth().apply { bottomMargin = dp(12) }
            addView(lock, LinearLayout.LayoutParams(dp(24), dp(24)))
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(12), 0, dp(8), 0)
                addView(text("Encrypted", 16f, com.google.android.material.R.attr.colorOnSurface))
                addView(sub)
                // Tapping the state pairs (again) -- the way to recover from a mismatched code -- or,
                // with teams, chooses who files are sealed to.
                setOnClickListener { if (encrypted) chooseSealTo() }
            }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(sw)
            addView(helpButton("Encrypted mode", ENCRYPTION_HELP))
        }
    }

    /** Who files are sealed to: the paired phone, or one of this phone's teams. */
    private fun chooseSealTo() {
        val teams = if (Entitlements.has(Entitlements.Feature.Teams)) Teams.store(this).all() else emptyList()
        if (teams.isEmpty()) {
            pair.launch(Intent(this, PairActivity::class.java))
            return
        }
        val key = PairingStore.current()
        val labels = listOf(key?.let { "Paired phone · ${it.code}" } ?: "Pair with a phone") +
            teams.map { "Team · ${it.name}" } + "Manage teams…"
        val current = teams.indexOfFirst { it.idHex == sealTeam()?.idHex } + 1
        com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
            .setTitle("Seal files to")
            .setSingleChoiceItems(labels.toTypedArray(), current) { d, which ->
                d.dismiss()
                when (which) {
                    0 -> {
                        setSealTo(null)
                        if (key == null) pair.launch(Intent(this, PairActivity::class.java))
                    }
                    labels.size - 1 -> startActivity(Intent(this, TeamsActivity::class.java))
                    else -> setSealTo(teams[which - 1].idHex)
                }
            }
            .show()
    }

    private fun setSealTo(idHex: String?) {
        sealTo = idHex
        getPreferences(MODE_PRIVATE).edit { putString(PREF_SEAL_TO, idHex) }
        refreshSecurity()
    }

    private fun refreshSecurity() {
        val key = PairingStore.current()
        val team = sealTeam()
        val state = when {
            !encrypted -> "Off"
            team != null -> "Team · ${team.name}"
            key != null -> "Paired · ${key.code}"
            else -> "Not paired · tap to pair"
        }
        securityRows.forEach { (sw, sub) ->
            if (sw.isChecked != encrypted) sw.isChecked = encrypted
            sub.text = state
        }
        locks.forEach { setLock(it, encrypted, animate = true) }
    }

    /** The lock closes and lights up when encrypted mode goes on -- with a turn, so it's noticed. */
    private fun setLock(lock: ImageView, on: Boolean, animate: Boolean) {
        if (lock.tag == on) return
        lock.tag = on
        val apply = {
            lock.setImageResource(if (on) R.drawable.ic_lock else R.drawable.ic_lock_open)
            lock.imageTintList = ColorStateList.valueOf(themeColor(
                if (on) androidx.appcompat.R.attr.colorPrimary
                else com.google.android.material.R.attr.colorOnSurfaceVariant,
            ))
        }
        if (!animate || !Motion.enabled) {
            apply()
            return
        }
        lock.animate().scaleX(0.6f).scaleY(0.6f).rotation(if (on) -20f else 20f).setDuration(Motion.SHORT)
            .setInterpolator(Motion.EXIT).withEndAction {
                apply()
                lock.animate().scaleX(1f).scaleY(1f).rotation(0f).setDuration(Motion.MEDIUM)
                    .setInterpolator(android.view.animation.OvershootInterpolator(3f)).start()
            }.start()
    }

    /**
     * Brings [tab]'s page in: the old page fades out, the new one fades through, and its contents
     * rise into place in reading order.
     */
    private fun show(tab: Int, animate: Boolean = true) {
        val into = pages[tab] ?: return
        if (tab == TAB_RECEIVED) refreshReceived()
        val out = pages.values.firstOrNull { it !== into && it.visibility == View.VISIBLE }
        pages.values.forEach { if (it !== into && it !== out) it.visibility = View.GONE }
        if (!animate) {
            out?.visibility = View.GONE
            into.visibility = View.VISIBLE
        }
        if (into.visibility == View.VISIBLE && out == null) return
        Motion.fadeThrough(out, into) {
            entrance[tab]?.invoke()?.let { Motion.staggerIn(it, risePx = dp(20).toFloat()) }
        }
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

    /** A tablet, by its shorter side: it gets the rail, bigger pictures, and grouped content. */
    private val tablet get() = resources.configuration.smallestScreenWidthDp >= 600

    /** Wide enough, on its side, for the picture and the words to stand side by side. */
    private val twoPane get() = tablet && resources.configuration.screenWidthDp >= 840 &&
        resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE

    /**
     * The one picture on an action page, animated ([HeroView]). It takes the space the page has
     * left, up to its own size, so a short screen shrinks it rather than pushing the buttons off.
     */
    private fun hero(view: HeroView) = view.apply {
        if (tablet) maxSidePx = dp(if (twoPane) 340 else 300)
        layoutParams = LinearLayout.LayoutParams(WRAP, 0, 1f).apply { gravity = Gravity.CENTER_HORIZONTAL }
    }

    /**
     * An action page: header at the top, then the picture, words and buttons. On a phone the
     * buttons sit at the bottom, under the thumb. On a tablet the whole group is centred together
     * -- buttons a long way below their words read as unrelated -- and on a wide tablet the picture
     * stands to the left of the words and buttons.
     */
    private fun actionPage(tab: Int, header: View, middle: List<View>, actions: List<View>): View {
        entrance[tab] = { middle + actions }
        val column = { views: List<View>, pad: Int ->
            LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER
                setPadding(dp(pad), dp(8), dp(pad), dp(8))
                views.forEach { addView(it) }
            }
        }
        val actionBox = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), 0, dp(20), dp(16))
            actions.forEach { addView(it) }
        }
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(header)
            when {
                twoPane -> addView(LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    val picture = middle.first()
                    picture.layoutParams = LinearLayout.LayoutParams(WRAP, WRAP).apply { gravity = Gravity.CENTER }
                    addView(android.widget.FrameLayout(context).apply {
                        addView(picture, android.widget.FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER))
                    }, LinearLayout.LayoutParams(0, MATCH, 1f))
                    // The words read from the left beside a picture, not centred under one.
                    middle.drop(1).forEach { (it as? TextView)?.gravity = Gravity.START }
                    addView(LinearLayout(context).apply {
                        orientation = LinearLayout.VERTICAL
                        gravity = Gravity.CENTER_VERTICAL
                        setPadding(dp(8), 0, 0, 0)
                        middle.drop(1).forEach { addView(it) }
                        addView(space(28))
                        addView(actionBox.apply { setPadding(0, 0, 0, dp(16)) })
                    }, LinearLayout.LayoutParams(0, MATCH, 1f))
                }, LinearLayout.LayoutParams(MATCH, 0, 1f))
                tablet -> addView(column(middle + space(32) + actionBox, 28), LinearLayout.LayoutParams(MATCH, 0, 1f))
                else -> {
                    addView(column(middle, 28), LinearLayout.LayoutParams(MATCH, 0, 1f))
                    addView(actionBox)
                }
            }
        }
    }

    private fun centred(v: android.widget.TextView) = v.apply { gravity = Gravity.CENTER }

    private fun sendPage(): View {
        speedLabel = button("", R.drawable.ic_tune, Style.Text) {
            nav.selectedItemId = TAB_SETTINGS
        }.apply { layoutParams = matchWidth(dp(4)) }
        updateSpeedLabel()
        return actionPage(
            TAB_SEND,
            header("Send", "How it works", HOW_IT_WORKS),
            listOf(
                hero(LightGridView(this)),
                space(20),
                centred(title("Send files", 26f)),
                space(8),
                centred(text("They appear on this screen as light for the other phone to scan. " +
                    "One file at a time is free. Pro sends several together; a receiving phone needs Pro for batches too.")),
            ),
            listOf(
                securityRow(),
                button("Choose files", R.drawable.ic_file) {
                    withPairing { pickFile.launch(arrayOf("*/*")) }
                }.apply { layoutParams = matchWidth() },
                button("Send a test file", R.drawable.ic_bolt, Style.Tonal) { send(emptyList()) }
                    .apply { layoutParams = matchWidth(dp(10)) },
                speedLabel,
            ),
        )
    }

    private fun updateSpeedLabel() {
        if (::speedLabel.isInitialized) speedLabel.text = "Speed: ${speed.label}"
    }

    private fun receivePage(): View = actionPage(
        TAB_RECEIVE,
        header("Receive", "Tips", RECEIVE_HELP),
        listOf(
            hero(ScanView(this)),
            space(20),
            centred(title("Receive a file", 26f)),
            space(8),
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
        val summary = text("", 13f).apply { setPadding(dp(20), 0, dp(20), dp(4)) }
        receivedSummary = summary
        val label = text("", 16f, com.google.android.material.R.attr.colorOnSurface)
        selectionLabel = label
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(4), dp(4), dp(8), dp(4))
            background = GradientDrawable().apply {
                cornerRadius = dp(28).toFloat()
                setColor(themeColor(com.google.android.material.R.attr.colorSecondaryContainer))
            }
            addView(iconButton(R.drawable.ic_close, "Cancel selection") { clearSelection() })
            addView(label, LinearLayout.LayoutParams(0, WRAP, 1f))
            addView(iconButton(R.drawable.ic_select_all, "Select all") { selectAll() })
            addView(iconButton(R.drawable.ic_share, "Share") {
                if (Entitlements.check(this@MainActivity, Entitlements.Feature.BulkManage))
                    shareAll(this@MainActivity, selected.toList(), "${selected.size} files")
            })
            // Passed on to another phone, as one batch: a file that reached one person in the
            // field often needs to reach the next.
            addView(iconButton(R.drawable.ic_upload, "Send with FileFlow") {
                if (!Entitlements.check(this@MainActivity, Entitlements.Feature.BulkManage)) return@iconButton
                val uris = selected.toList()
                clearSelection()
                send(uris)
            })
            addView(iconButton(R.drawable.ic_delete, "Delete") { confirmDeleteSelected() })
            visibility = View.GONE
        }
        selectionBar = bar
        entrance[TAB_RECEIVED] = { listOf<View>(summary) + (0 until list.childCount).map { list.getChildAt(it) } }
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(header("Received", "Received files", RECEIVED_HELP))
            addView(summary)
            addView(bar, matchWidth().apply { setMargins(dp(16), 0, dp(16), dp(4)) })
            // A list may scroll -- it is the one thing here that grows.
            addView(ScrollView(context).apply { addView(list) },
                LinearLayout.LayoutParams(MATCH, 0, 1f))
        }
    }

    /** A received file, as the list shows it. */
    private class ReceivedItem(
        val uri: android.net.Uri, val name: String, val bytes: Long, val mime: String, val addedS: Long,
    )

    private var receivedItems: List<ReceivedItem> = emptyList()
    private val selected = LinkedHashSet<android.net.Uri>()
    private val rows = HashMap<android.net.Uri, com.google.android.material.card.MaterialCardView>()
    private var selectionBar: View? = null
    private var selectionLabel: TextView? = null
    private var receivedSummary: TextView? = null

    /** Back leaves selection before it leaves the app. */
    private val clearSelectionOnBack = object : androidx.activity.OnBackPressedCallback(false) {
        override fun handleOnBackPressed() = clearSelection()
    }

    /** Files this app saved to Downloads/FileFlow, newest first. */
    private fun refreshReceived() {
        val list = receivedList ?: return
        list.removeAllViews()
        rows.clear()
        thumbnails.queue.clear()  // pictures for the rows just removed
        val items = ArrayList<ReceivedItem>()
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
                while (c.moveToNext() && items.size < MAX_LISTED) {
                    items += ReceivedItem(
                        ContentUris.withAppendedId(MediaStore.Downloads.EXTERNAL_CONTENT_URI, c.getLong(0)),
                        c.getString(1) ?: "file", c.getLong(2), c.getString(3) ?: "application/octet-stream",
                        c.getLong(4),
                    )
                }
            }
        }
        receivedItems = items
        selected.retainAll(items.map { it.uri }.toSet())  // a deleted file is no longer selected
        if (items.isEmpty()) {
            list.addView(emptyState(
                R.drawable.ic_folder, "Nothing here yet",
                "Files you receive are kept here, and in Downloads › ${LiveDecoder.DOWNLOADS_SUBDIR}.",
                button("Start receiving", R.drawable.ic_camera, Style.Tonal) { nav.selectedItemId = TAB_RECEIVE },
            ), matchWidth())
            updateSelection()
            return
        }
        items.forEach { item ->
            val whenText = android.text.format.DateUtils.getRelativeTimeSpanString(
                item.addedS * 1000, System.currentTimeMillis(),
                android.text.format.DateUtils.MINUTE_IN_MILLIS,
            )
            val mime = item.mime
            val card = row(
                R.drawable.ic_file, item.name, "${TransmitActivity.formatBytes(item.bytes)} · $whenText",
                onLongClick = {
                    if (Entitlements.has(Entitlements.Feature.BulkManage)) toggle(item.uri)
                    else singleFileActions(item.uri, item.name, item.mime)
                },
                thumbnailOf = if (mime.startsWith("image/") || mime.startsWith("video/")) item.uri else null,
            ) {
                if (selected.isNotEmpty()) {
                    toggle(item.uri)
                } else {
                    val view = Intent(Intent.ACTION_VIEW)
                        .setDataAndType(item.uri, mime)
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    runCatching { startActivity(Intent.createChooser(view, item.name)) }
                }
            } as com.google.android.material.card.MaterialCardView
            card.isCheckable = true
            card.isChecked = item.uri in selected
            rows[item.uri] = card
            list.addView(card, matchWidth(dp(8)))
        }
        if (items.size == MAX_LISTED) {
            list.addView(centred(text("Showing the newest $MAX_LISTED. The rest are in your Files app, " +
                "in Downloads › ${LiveDecoder.DOWNLOADS_SUBDIR}.", 13f)).apply { setPadding(0, dp(12), 0, 0) },
                matchWidth())
        }
        list.addView(centred(text("Press and hold for file actions. Pro adds selecting, sharing, resending and deleting several at once.", 13f))
            .apply { setPadding(0, dp(12), 0, 0) }, matchWidth())
        updateSelection()
    }

    private fun toggle(uri: android.net.Uri) {
        if (!Entitlements.check(this, Entitlements.Feature.BulkManage)) return
        if (!selected.remove(uri)) selected += uri
        rows[uri]?.isChecked = uri in selected
        updateSelection()
    }

    private fun selectAll() {
        if (!Entitlements.check(this, Entitlements.Feature.BulkManage)) return
        receivedItems.forEach { selected += it.uri }
        rows.values.forEach { it.isChecked = true }
        updateSelection()
    }

    private fun clearSelection() {
        selected.clear()
        rows.values.forEach { it.isChecked = false }
        updateSelection()
    }

    /** The selection bar, or the list's summary when nothing is selected. */
    private fun updateSelection() {
        val selecting = selected.isNotEmpty()
        clearSelectionOnBack.isEnabled = selecting
        selectionBar?.let { Motion.reveal(it, selecting) }
        // Each row's chevron gives way to the selection tick, which sits where it does.
        rows.values.forEach { card ->
            card.findViewWithTag<View>(CHEVRON)?.visibility = if (selecting) View.INVISIBLE else View.VISIBLE
        }
        receivedSummary?.visibility = if (selecting || receivedItems.isEmpty()) View.GONE else View.VISIBLE
        val bytes = receivedItems.filter { it.uri in selected }.sumOf { it.bytes }
        selectionLabel?.text = "${selected.size} selected\n${TransmitActivity.formatBytes(bytes)}"
        receivedSummary?.text = "${receivedItems.size} ${if (receivedItems.size == 1) "file" else "files"} · " +
            TransmitActivity.formatBytes(receivedItems.sumOf { it.bytes })
    }

    private fun confirmDeleteSelected() {
        if (!Entitlements.check(this, Entitlements.Feature.BulkManage)) return
        val uris = selected.toList()
        if (uris.isEmpty()) return
        com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
            .setTitle(if (uris.size == 1) "Delete this file?" else "Delete ${uris.size} files?")
            .setMessage("They will be removed from Downloads › ${LiveDecoder.DOWNLOADS_SUBDIR}.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Delete") { _, _ ->
                if (!Entitlements.check(this, Entitlements.Feature.BulkManage)) return@setPositiveButton
                selected.clear()
                deleteFiles(uris)
            }
            .show()
    }

    /** Individual file actions stay available in Free; only the multi-file workflow is paid. */
    private fun singleFileActions(uri: android.net.Uri, name: String, mime: String) {
        com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
            .setTitle(name)
            .setItems(arrayOf("Open", "Share", "Send with FileFlow", "Delete", "Select multiple · Pro")) { _, action ->
                when (action) {
                    0 -> runCatching { startActivity(Intent.createChooser(Intent(Intent.ACTION_VIEW)
                        .setDataAndType(uri, mime).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), name)) }
                    1 -> shareAll(this, listOf(uri), name)
                    2 -> send(listOf(uri))
                    3 -> com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
                        .setTitle("Delete this file?").setMessage(name).setNegativeButton("Cancel", null)
                        .setPositiveButton("Delete") { _, _ -> deleteFiles(listOf(uri)) }.show()
                    4 -> if (Entitlements.check(this, Entitlements.Feature.BulkManage)) toggle(uri)
                }
            }.show()
    }

    /** A round, tinted icon button, for the selection bar. */
    private fun iconButton(icon: Int, description: String, onClick: () -> Unit) =
        MaterialButton(this, null, com.google.android.material.R.attr.materialIconButtonStyle).apply {
            setIconResource(icon)
            contentDescription = description
            tooltipText = description
            setOnClickListener { onClick() }
        }

    /**
     * Deletes received files. This install's own are deleted outright; one saved by an earlier
     * install is not the app's to delete, so Android asks the person -- once, for all of them.
     */
    private fun deleteFiles(uris: List<android.net.Uri>) {
        val refused = uris.filter { uri ->
            !runCatching { contentResolver.delete(uri, null, null) > 0 }.getOrDefault(false)
        }
        if (refused.isNotEmpty()) {
            runCatching {
                val request = MediaStore.createDeleteRequest(contentResolver, refused)
                confirmDelete.launch(androidx.activity.result.IntentSenderRequest.Builder(request).build())
            }.onFailure {
                android.widget.Toast.makeText(this, "Delete it from the Files app.",
                    android.widget.Toast.LENGTH_LONG).show()
            }
        }
        refreshReceived()
    }

    private val confirmDelete = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.StartIntentSenderForResult(),
    ) { refreshReceived() }

    /**
     * Loads list thumbnails: two at a time, however many rows a refresh builds. It used to start a
     * thread per row on every visit to the tab, up to a hundred at once.
     */
    private val thumbnails = java.util.concurrent.ThreadPoolExecutor(
        2, 2, 0L, java.util.concurrent.TimeUnit.MILLISECONDS, java.util.concurrent.LinkedBlockingQueue(),
    )

    override fun onDestroy() {
        super.onDestroy()
        thumbnails.shutdownNow()
    }

    private fun settingsPage(prefs: android.content.SharedPreferences): View {
        val toggle = segmented(Speed.entries, speed, { it.label }) {
            speed = it
            prefs.edit { putString(PREF_SPEED, speed.name) }
            updateSpeedLabel()
        }
        val compressionToggle = segmented(Envelope.Mode.entries, compression, { it.name }) {
            compression = it
            prefs.edit { putString(PREF_COMPRESSION, compression.name) }
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
            addView(card(
                LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    addView(title("Compression", 18f), LinearLayout.LayoutParams(0, WRAP, 1f))
                    addView(helpButton("Compression", COMPRESSION_HELP))
                },
                space(8),
                compressionToggle.apply { layoutParams = matchWidth() },
            ), matchWidth(dp(12)))
            addView(row(R.drawable.ic_group, "Teams", "Encrypted sending to a whole group at once") {
                if (Entitlements.check(this@MainActivity, Entitlements.Feature.Teams)) {
                    startActivity(Intent(this@MainActivity, TeamsActivity::class.java))
                }
            }, matchWidth(dp(12)))
            addView(row(R.drawable.ic_tune, "FileFlow Pro", "Your account, purchases, and offline Pro access") {
                startActivity(Intent(this@MainActivity, ProActivity::class.java))
            }, matchWidth(dp(12)))
            addView(row(R.drawable.ic_help, "How it works", null) {
                showHelp("How it works", HOW_IT_WORKS)
            }, matchWidth(dp(12)))
            addView(row(R.drawable.ic_help, "Privacy & accounts", "Your data, verification, and account deletion") {
                runCatching { startActivity(Intent(Intent.ACTION_VIEW, android.net.Uri.parse(BuildConfig.ACCOUNT_ORIGIN + "/privacy.html"))) }
                    .onFailure { android.widget.Toast.makeText(this@MainActivity, "Open ${BuildConfig.ACCOUNT_ORIGIN}/privacy.html in your browser.", android.widget.Toast.LENGTH_LONG).show() }
            }, matchWidth(dp(12)))
            addView(row(R.drawable.ic_tune, "Device check", "What this phone's camera and screen can do") {
                startActivity(Intent(this@MainActivity, ProbeActivity::class.java))
            }, matchWidth(dp(8)))
            addView(row(R.drawable.ic_help, "Contact support", "fileflowsupport@gmail.com") {
                runCatching { startActivity(Intent(Intent.ACTION_SENDTO, android.net.Uri.parse("mailto:fileflowsupport@gmail.com"))) }
                    .onFailure { android.widget.Toast.makeText(this@MainActivity, "Email fileflowsupport@gmail.com for support.", android.widget.Toast.LENGTH_LONG).show() }
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
            // Scrolls: on a small phone, or with the system's text made large, it doesn't all fit.
            addView(ScrollView(context).apply { addView(body) }, LinearLayout.LayoutParams(MATCH, 0, 1f))
            entrance[TAB_SETTINGS] = { (0 until body.childCount).map { body.getChildAt(it) } }
        }
    }

    /** One choice of several, as a row of segments, [selected] checked. */
    private fun <T> segmented(
        entries: List<T>, selected: T, label: (T) -> String, onPick: (T) -> Unit,
    ): MaterialButtonToggleGroup =
        MaterialButtonToggleGroup(this).apply {
            isSingleSelection = true
            isSelectionRequired = true
            entries.forEach { s ->
                // Outlined style, untouched: inside a toggle group it is what draws the checked
                // segment filled and the others clear.
                addView(MaterialButton(
                    context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle,
                ).apply {
                    id = View.generateViewId()
                    tag = s
                    text = label(s)
                    isAllCaps = false
                    textSize = 14f
                    minHeight = dp(48)
                    maxLines = 1
                    // The default 24 dp side padding truncated "Steady" to "Ste…".
                    setPaddingRelative(dp(4), paddingTop, dp(4), paddingBottom)
                    // Material 3's segmented button: the chosen one filled, ticked, at full contrast.
                    // The outlined style alone left it a shade darker, with dimmer text than the rest.
                    val checked = intArrayOf(android.R.attr.state_checked)
                    backgroundTintList = ColorStateList(arrayOf(checked, intArrayOf()), intArrayOf(
                        themeColor(androidx.appcompat.R.attr.colorPrimary), android.graphics.Color.TRANSPARENT,
                    ))
                    val ink = ColorStateList(arrayOf(checked, intArrayOf()), intArrayOf(
                        themeColor(com.google.android.material.R.attr.colorOnPrimary),
                        themeColor(com.google.android.material.R.attr.colorOnSurface),
                    ))
                    typeface = android.graphics.Typeface.create("sans-serif-medium", android.graphics.Typeface.NORMAL)
                    setTextColor(ink)
                    iconTint = ink
                    iconSize = dp(18)
                    iconPadding = dp(6)
                    iconGravity = MaterialButton.ICON_GRAVITY_TEXT_START
                    addOnCheckedChangeListener { b, on ->
                        b.icon = if (on) androidx.core.content.ContextCompat.getDrawable(context, R.drawable.ic_check) else null
                    }
                }, LinearLayout.LayoutParams(0, WRAP, 1f))
            }
            check((0 until childCount).map { getChildAt(it) }.first { it.tag == selected }.id)
            addOnButtonCheckedListener { group, id, checked ->
                @Suppress("UNCHECKED_CAST")
                if (checked) onPick(group.findViewById<View>(id).tag as T)
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
                thumbnails.execute {
                    val bmp = runCatching {
                        contentResolver.loadThumbnail(thumbnailOf, android.util.Size(dp(96), dp(96)), null)
                    }.getOrNull() ?: return@execute
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
                }
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
                tag = CHEVRON
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
            Motion.pressable(this, 0.98f)
        }

    companion object {
        const val ACTION_RECEIVE = "dev.fileflow.action.RECEIVE"
        const val ACTION_SEND_FILE = "dev.fileflow.action.SEND_FILE"
        /** Opens on the Received tab: from a received batch's result screen. */
        const val ACTION_SHOW_RECEIVED = "dev.fileflow.action.SHOW_RECEIVED"

        /** Shares several received files at once, through the system share sheet. */
        fun shareAll(context: android.content.Context, uris: List<android.net.Uri>, title: String) {
            if (uris.isEmpty()) return
            if (uris.size > 1 && !Entitlements.check(context, Entitlements.Feature.BulkManage)) return
            val send = if (uris.size == 1) {
                Intent(Intent.ACTION_SEND).putExtra(Intent.EXTRA_STREAM, uris[0])
            } else {
                Intent(Intent.ACTION_SEND_MULTIPLE).putParcelableArrayListExtra(Intent.EXTRA_STREAM, ArrayList(uris))
            }
            // A common type when they share one, else any; clip data carries the read grant for all.
            val types = uris.map { context.contentResolver.getType(it) ?: "application/octet-stream" }.distinct()
            send.type = types.singleOrNull() ?: "*/*"
            val clip = android.content.ClipData.newRawUri(title, uris[0])
            uris.drop(1).forEach { clip.addItem(android.content.ClipData.Item(it)) }
            send.clipData = clip
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            runCatching { context.startActivity(Intent.createChooser(send, title)) }
        }
        private const val PREF_SPEED = "speed"
        private const val PREF_ENCRYPTED = "encrypted"
        private const val PREF_SEAL_TO = "seal_to"

        /** Share-sheet sends use the same choices as Send, never caller-supplied security extras. */
        internal fun configureSharedSend(context: android.content.Context, intent: Intent) {
            val prefs = context.getSharedPreferences(MainActivity::class.java.simpleName, MODE_PRIVATE)
            val speed = Speed.named(prefs.getString(PREF_SPEED, null))
            val encrypted = prefs.getBoolean(PREF_ENCRYPTED, false)
            intent.putExtra("cols", speed.cols)
                .putExtra("rows", speed.rows)
                .putExtra("stateRate", speed.statesPerSecond)
                .putExtra("colours", speed.colours)
                .putExtra("encrypted", encrypted)
                .putExtra("team", if (encrypted && Entitlements.has(Entitlements.Feature.Teams)) {
                    prefs.getString(PREF_SEAL_TO, null)
                } else null)
                .putExtra("compression", prefs.getString(PREF_COMPRESSION, null))
        }

        /**
         * A team's key was changed ([Team.rotated]) and with it its id: if files were being sealed to
         * it, they now go to the new key. (MainActivity's preferences: getPreferences names the
         * file after the activity.)
         */
        fun teamRekeyed(context: android.content.Context, oldIdHex: String, newIdHex: String) {
            val prefs = context.getSharedPreferences(MainActivity::class.java.simpleName, MODE_PRIVATE)
            if (prefs.getString(PREF_SEAL_TO, null) == oldIdHex) prefs.edit { putString(PREF_SEAL_TO, newIdHex) }
        }
        private const val PREF_COMPRESSION = "compression"
        private const val PREF_TAB = "tab"
        private const val TAB_SEND = 1
        private const val TAB_RECEIVE = 2
        private const val TAB_RECEIVED = 3
        private const val TAB_SETTINGS = 4
        private const val MATCH = LinearLayout.LayoutParams.MATCH_PARENT
        private const val WRAP = LinearLayout.LayoutParams.WRAP_CONTENT

        /** Tags a list row's chevron, which selection hides. */
        private const val CHEVRON = "chevron"

        /** Received files listed at most; the rest are in the Files app. */
        private const val MAX_LISTED = 500

        private const val RECEIVED_HELP =
            "Verified files are saved to Downloads › ${LiveDecoder.DOWNLOADS_SUBDIR}.\n\n" +
                "Tap a file to open it. Press and hold to select it, then tap others to select " +
                "them too; share, delete, or send them on to another phone with FileFlow, " +
                "all at once."

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
                "isn't sealed to its pairing or one of its teams.\n\n" +
                "Teams (Settings › Teams) keep one key for a whole group, so one screen can send " +
                "to everyone in it at once. Tap Encrypted to choose a team to send to."

        private const val COMPRESSION_HELP =
            "Auto — files that compress, like documents, text and spreadsheets, are made smaller " +
                "before sending, so fewer bytes cross the light link. Photos, videos and archives " +
                "are already compressed, and a quick check sends those as they are.\n\n" +
                "Maximum — the strongest compression, for the smallest transfer. It takes longer " +
                "before sending starts, which pays off on big, very compressible files.\n\n" +
                "Off — every file is sent exactly as it is. Use it to compare speeds.\n\n" +
                "The receiving phone shows the link's speed, the file's effective speed, and the " +
                "time compression saved or cost. Only the sending phone needs this setting."

        private const val SPEED_HELP =
            "Fastest — eight colours at 30 frames a second: three times the data of black and " +
                "white. Needs a receiver whose camera sees colour sharply.\n\n" +
                "Fast — four colours at 30 frames a second: twice the data of black and white, and " +
                "much easier for a camera to read. Use it if the receiver says it can't read the " +
                "colours.\n\n" +
                "Steady — black and white. Slower, but for a camera or lighting colour doesn't " +
                "survive.\n\n" +
                "Only the sending phone needs this setting; the receiver works it out."
    }
}
