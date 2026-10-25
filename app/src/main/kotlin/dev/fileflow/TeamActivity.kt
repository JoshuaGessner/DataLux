package dev.fileflow

import android.content.Intent
import android.os.Bundle
import android.text.InputFilter
import android.text.InputType
import android.view.Gravity
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import dev.fileflow.secure.Member
import dev.fileflow.secure.Team
import dev.fileflow.secure.Teams
import dev.fileflow.ui.Ui.Style
import dev.fileflow.ui.Ui.button
import dev.fileflow.ui.Ui.card
import dev.fileflow.ui.Ui.dp
import dev.fileflow.ui.Ui.helpButton
import dev.fileflow.ui.Ui.matchWidth
import dev.fileflow.ui.Ui.space
import dev.fileflow.ui.Ui.text
import dev.fileflow.ui.Ui.title

/**
 * One team ([Team]): who is in it, and -- for the phone that leads it -- inviting, removing,
 * renaming and deleting. A member sees the list as it was when they joined, and can leave.
 */
class TeamActivity : AppCompatActivity() {

    private lateinit var body: LinearLayout
    private lateinit var heading: android.widget.TextView
    private var idHex: String = ""

    private val pair = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { refresh() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        idHex = intent.getStringExtra(EXTRA_TEAM).orEmpty()
        heading = title("", 24f)
        body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(8), dp(16), dp(20))
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(MaterialButton(
                    context, null, com.google.android.material.R.attr.materialIconButtonStyle,
                ).apply {
                    setIconResource(R.drawable.ic_close)
                    contentDescription = "Close"
                    setOnClickListener { finish() }
                })
                addView(heading, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
                addView(helpButton("Teams", TeamsActivity.HELP))
            })
            addView(body)
        }
        setContentView(ScrollView(this).apply { addView(col) }.padForSystemBars())
        // Filled in onResume, which also catches changes made while away (an invite just finished).
    }

    private fun team(): Team? = Teams.store(this).find(idHex)

    private fun refresh() {
        body.removeAllViews()
        val team = team() ?: run {
            finish()
            return
        }
        heading.text = team.name
        val leader = team.leader
        body.addView(text(when {
            team.isLeader -> "You lead this team. You invite people to it, and you can remove them."
            leader != null -> "Led by ${leader.name}."
            else -> "A team you joined."
        }).apply { setPadding(dp(4), dp(4), dp(4), dp(12)) })

        // Members, leader first.
        val rows = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val members = team.members.ifEmpty { listOf(Member(Profile.name(this), team.createdMs, leader = true, you = true)) }
        members.forEach { m -> rows.addView(memberRow(team, m)) }
        body.addView(card(
            title("Members · ${members.count { !it.needsNewKey }}", 18f),
            space(4),
            rows,
            text(if (team.isLeader) {
                "Tap someone to remove them from the team."
            } else {
                "The team as it was when you joined. New members show up on the leader's phone."
            }, 13f).apply { setPadding(0, dp(8), 0, 0) },
        ), matchWidth())

        if (team.isLeader) {
            if (team.members.any { it.needsNewKey }) {
                body.addView(text("The team has a new key. Invite everyone marked \"needs the new key\" again, " +
                    "one at a time.", 13f).apply { setPadding(dp(4), dp(12), dp(4), 0) })
            }
            body.addView(button("Invite someone", R.drawable.ic_add) {
                pair.launch(Intent(this, PairActivity::class.java).putExtra(PairActivity.EXTRA_INVITE, team.idHex))
            }, matchWidth(dp(16)))
            body.addView(button("Rename team", style = Style.Tonal) { rename(team) }, matchWidth(dp(8)))
            body.addView(button("Delete team", R.drawable.ic_delete, Style.Text) { delete(team) }, matchWidth(dp(8)))
        } else {
            body.addView(button("Leave team", R.drawable.ic_delete, Style.Tonal) { leave(team) }, matchWidth(dp(16)))
        }
    }

    private fun memberRow(team: Team, m: Member): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, dp(8), 0, dp(8))
        addView(text(m.name.ifEmpty { "Unnamed" } + if (m.you) " (you)" else "", 16f,
                     com.google.android.material.R.attr.colorOnSurface))
        val joined = android.text.format.DateFormat.getMediumDateFormat(context).format(java.util.Date(m.joinedMs))
        addView(text(when {
            m.leader -> "Leader · since $joined"
            m.needsNewKey -> "Needs the new key: invite again"
            else -> "Joined $joined"
        }, 13f))
        if (team.isLeader && !m.you) {
            val ripple = android.util.TypedValue()
            context.theme.resolveAttribute(android.R.attr.selectableItemBackground, ripple, true)
            setBackgroundResource(ripple.resourceId)
            setOnClickListener { remove(team, m) }
        }
    }

    /** Removing takes a new key: a key can't be taken back from a phone that has it. */
    private fun remove(team: Team, m: Member) {
        val others = team.members.count { !it.leader && !it.you && it !== m }
        MaterialAlertDialogBuilder(this)
            .setTitle("Remove ${m.name}?")
            .setMessage("A phone can't be made to forget a key, so removing someone gives the team a NEW " +
                "key. ${m.name} can't open anything sent to the team after this.\n\n" +
                (if (others > 0) "The $others other ${if (others == 1) "member" else "members"} " +
                    "will need the new key too: invite each of them again, face to face. Until then, " +
                    "they can't open what you send.\n\n" else "") +
                "Files sent before stay readable to everyone who had them.")
            .setPositiveButton("Remove and change key") { _, _ ->
                val next = team.without(m).rotated()
                Teams.store(this).replace(team.idHex, next)
                MainActivity.teamRekeyed(this, team.idHex, next.idHex)
                idHex = next.idHex
                refresh()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun rename(team: Team) {
        val input = EditText(this).apply {
            setText(team.name)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS
            filters = arrayOf(InputFilter.LengthFilter(Team.MAX_NAME_CHARS))
            isSingleLine = true
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("Rename team")
            .setMessage("People you invite from now on see the new name. Members already in it keep " +
                "the name they joined with.")
            .setView(LinearLayout(this).apply {
                setPadding(dp(24), 0, dp(24), 0)
                addView(input, matchWidth())
            })
            .setPositiveButton("Rename") { _, _ ->
                Teams.store(this).replace(team.idHex, team.renamed(input.text.toString()))
                refresh()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun delete(team: Team) {
        MaterialAlertDialogBuilder(this)
            .setTitle("Delete ${team.name}?")
            .setMessage("This phone forgets the team and its key. Members keep their copies and can still " +
                "send to each other; to stop that, remove them first.")
            .setPositiveButton("Delete") { _, _ ->
                Teams.store(this).remove(team.idHex)
                finish()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun leave(team: Team) {
        MaterialAlertDialogBuilder(this)
            .setTitle("Leave ${team.name}?")
            .setMessage("This phone forgets the team's key, and can no longer open what's sent to the " +
                "team. To be in it again, the leader invites you.")
            .setPositiveButton("Leave") { _, _ ->
                Teams.store(this).remove(team.idHex)
                finish()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    override fun onResume() {
        super.onResume()
        if (::body.isInitialized) refresh()
    }

    companion object {
        /** The [Team.idHex] of the team to show. */
        const val EXTRA_TEAM = "team"
    }
}
