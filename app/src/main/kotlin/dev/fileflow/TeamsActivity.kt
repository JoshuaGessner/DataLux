package dev.fileflow

import android.content.Intent
import android.os.Bundle
import android.text.InputFilter
import android.text.InputType
import android.view.Gravity
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import dev.fileflow.secure.Team
import dev.fileflow.secure.Teams
import dev.fileflow.ui.Ui.Style
import dev.fileflow.ui.Ui.avatar
import dev.fileflow.ui.Ui.button
import dev.fileflow.ui.Ui.emptyState
import dev.fileflow.ui.Ui.capped
import dev.fileflow.ui.Ui.card
import dev.fileflow.ui.Ui.cardHeader
import dev.fileflow.ui.Ui.dp
import dev.fileflow.ui.Ui.helpButton
import dev.fileflow.ui.Ui.matchWidth
import dev.fileflow.ui.Ui.space
import dev.fileflow.ui.Ui.text
import dev.fileflow.ui.Ui.title

/**
 * The teams this phone is in ([Team]), and the name its person goes by in them. Tapping a team
 * opens it ([TeamActivity]); making one and joining one start here. Inviting and joining both
 * happen on the pairing screen, face to face.
 */
class TeamsActivity : AppCompatActivity() {

    private lateinit var list: LinearLayout
    private lateinit var nameView: TextView
    private lateinit var avatarSlot: LinearLayout

    private val pair = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        refresh()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        nameView = title("", 18f)
        avatarSlot = LinearLayout(this)
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
                addView(title("Teams", 24f), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
                addView(helpButton("Teams", HELP))
            })
            addView(space(8))
            // Who this phone is, to the teams it joins.
            addView(card(
                text("Your name", 13f),
                LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    addView(avatarSlot)
                    addView(nameView, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply {
                        marginStart = dp(12)
                    })
                    addView(button("Change", style = Style.Text) { changeName() })
                },
                text("What the people in your teams see when you join.", 13f),
            ), matchWidth())
            addView(list, matchWidth(dp(8)))
            addView(button("New team", R.drawable.ic_add) { newTeam() }, matchWidth(dp(16)))
            addView(button("Join a team", R.drawable.ic_group, Style.Tonal) {
                pair.launch(Intent(this@TeamsActivity, PairActivity::class.java).putExtra(PairActivity.EXTRA_JOIN, true))
            }, matchWidth(dp(8)))
        }
        setContentView(ScrollView(this).apply { addView(capped(col)) }.padForSystemBars())
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun refresh() {
        nameView.text = Profile.name(this)
        avatarSlot.removeAllViews()
        avatarSlot.addView(avatar(Profile.name(this), 44))
        list.removeAllViews()
        val teams = Teams.store(this).all()
        if (teams.isEmpty()) {
            list.addView(emptyState(R.drawable.ic_group, "No teams yet",
                "Make one and invite the others, or join someone else's. Everyone in a team can " +
                    "send encrypted files to all the others at once."))
        }
        teams.forEach { team ->
            val count = team.members.count { !it.needsNewKey }.coerceAtLeast(1)
            val who = if (team.isLeader) "You lead it" else "Led by ${team.leader?.name ?: "someone else"}"
            list.addView(card(
                cardHeader(R.drawable.ic_group, team.name, android.widget.ImageView(this).apply {
                    setImageResource(R.drawable.ic_chevron)
                }),
                text("$who · $count ${if (count == 1) "member" else "members"}", 13f).apply {
                    setPadding(dp(58), 0, 0, 0)
                },
            ).apply {
                setOnClickListener {
                    startActivity(Intent(this@TeamsActivity, TeamActivity::class.java)
                        .putExtra(TeamActivity.EXTRA_TEAM, team.idHex))
                }
                dev.fileflow.ui.Motion.pressable(this, 0.98f)
            }, matchWidth(dp(8)))
        }
        dev.fileflow.ui.Motion.staggerIn((0 until list.childCount).map { list.getChildAt(it) },
                                         risePx = dp(16).toFloat())
    }

    private fun nameInput(initial: String, hint: String) = EditText(this).apply {
        setText(initial)
        this.hint = hint
        inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS
        filters = arrayOf(InputFilter.LengthFilter(Team.MAX_NAME_CHARS))
        isSingleLine = true
    }

    private fun dialogView(input: EditText) = LinearLayout(this).apply {
        setPadding(dp(24), 0, dp(24), 0)
        addView(input, matchWidth())
    }

    private fun changeName() {
        val input = nameInput(Profile.name(this), "Your name")
        MaterialAlertDialogBuilder(this)
            .setTitle("Your name")
            .setMessage("Shown to a team's leader and members when you join. Teams you're already in " +
                "keep the name you joined with.")
            .setView(dialogView(input))
            .setPositiveButton("Save") { _, _ ->
                if (Team.cleanName(input.text.toString()).isNotEmpty()) {
                    Profile.setName(this, input.text.toString())
                    refresh()
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun newTeam() {
        val input = nameInput("", "Field crew")
        MaterialAlertDialogBuilder(this)
            .setTitle("New team")
            .setMessage("A name everyone in it will recognise. You lead the team: you invite people " +
                "to it, and can remove them.")
            .setView(dialogView(input))
            .setPositiveButton("Make") { _, _ ->
                val name = Team.cleanName(input.text.toString())
                if (name.isNotEmpty()) {
                    val team = Team.create(name, Profile.name(this))
                    Teams.store(this).add(team)
                    startActivity(Intent(this, TeamActivity::class.java).putExtra(TeamActivity.EXTRA_TEAM, team.idHex))
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    companion object {
        const val HELP =
            "A team is a group that sends encrypted files to each other — one transfer, read by " +
                "every member's camera at once, opens on all their phones and nobody else's.\n\n" +
                "Whoever makes a team leads it. To add someone, the leader opens the team and taps " +
                "Invite someone; the new member taps Join a team; they hold the two phones face to " +
                "face. The phones pair, both show the same six-digit code, and then the team's key " +
                "passes across, sealed so only that phone can open it. The new member's name comes " +
                "back to the leader's list.\n\n" +
                "To send to a team, turn on Encrypted and tap it to choose the team. To receive, " +
                "just turn on Encrypted: files for any team you're in open by themselves.\n\n" +
                "Removing someone gives the team a new key, because a phone can't be made to forget " +
                "one. Everyone else is then invited again."
    }
}
