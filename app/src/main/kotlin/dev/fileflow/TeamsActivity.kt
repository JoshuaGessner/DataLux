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
import dev.fileflow.secure.Team
import dev.fileflow.secure.Teams
import dev.fileflow.ui.Ui.Style
import dev.fileflow.ui.Ui.button
import dev.fileflow.ui.Ui.card
import dev.fileflow.ui.Ui.cardHeader
import dev.fileflow.ui.Ui.dp
import dev.fileflow.ui.Ui.helpButton
import dev.fileflow.ui.Ui.matchWidth
import dev.fileflow.ui.Ui.space
import dev.fileflow.ui.Ui.text
import dev.fileflow.ui.Ui.title

/**
 * The teams this phone is in ([Team]): make one, invite someone to it, join one, leave one.
 * Inviting and joining both happen on the pairing screen, face to face.
 */
class TeamsActivity : AppCompatActivity() {

    private lateinit var list: LinearLayout

    private val pair = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        refresh()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
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
            addView(list)
            addView(button("New team", R.drawable.ic_add) { newTeam() }, matchWidth(dp(16)))
            addView(button("Join a team", R.drawable.ic_group, Style.Tonal) {
                pair.launch(Intent(this@TeamsActivity, PairActivity::class.java).putExtra(PairActivity.EXTRA_JOIN, true))
            }, matchWidth(dp(8)))
        }
        setContentView(ScrollView(this).apply { addView(col) }.padForSystemBars())
        refresh()
    }

    private fun refresh() {
        list.removeAllViews()
        val teams = Teams.store(this).all()
        if (teams.isEmpty()) {
            list.addView(text("No teams yet. Make one and invite the others, or join someone else's.").apply {
                setPadding(dp(4), dp(8), dp(4), dp(8))
            })
        }
        teams.forEach { team ->
            val made = android.text.format.DateFormat.getMediumDateFormat(this).format(java.util.Date(team.createdMs))
            list.addView(card(
                cardHeader(R.drawable.ic_group, team.name, MaterialButton(
                    this, null, com.google.android.material.R.attr.materialIconButtonStyle,
                ).apply {
                    setIconResource(R.drawable.ic_delete)
                    contentDescription = "Leave ${team.name}"
                    setOnClickListener { leave(team) }
                }),
                text("Since $made", 13f).apply { setPadding(dp(58), 0, 0, dp(8)) },
                button("Invite someone", R.drawable.ic_add, Style.Tonal) {
                    pair.launch(Intent(this, PairActivity::class.java).putExtra(PairActivity.EXTRA_INVITE, team.idHex))
                }.apply { layoutParams = matchWidth(dp(4)) },
            ), matchWidth(dp(8)))
        }
    }

    private fun newTeam() {
        val input = EditText(this).apply {
            hint = "Field crew"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS
            filters = arrayOf(InputFilter.LengthFilter(Team.MAX_NAME_CHARS))
            isSingleLine = true
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("New team")
            .setMessage("A name everyone in it will recognise.")
            .setView(LinearLayout(this).apply {
                setPadding(dp(24), 0, dp(24), 0)
                addView(input, matchWidth())
            })
            .setPositiveButton("Make") { _, _ ->
                val name = Team.cleanName(input.text.toString())
                if (name.isNotEmpty()) {
                    Teams.store(this).add(Team.create(name))
                    refresh()
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun leave(team: Team) {
        MaterialAlertDialogBuilder(this)
            .setTitle("Leave ${team.name}?")
            .setMessage("This phone forgets the team's key, and can no longer open what's sent to the " +
                "team. To be in it again, someone in it invites you.")
            .setPositiveButton("Leave") { _, _ ->
                Teams.store(this).remove(team.idHex)
                refresh()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    companion object {
        private const val HELP =
            "A team is a group that sends encrypted files to each other — one transfer, read by " +
                "every member's camera at once, opens on all their phones and nobody else's.\n\n" +
                "Each team has one key, kept on this phone and locked to it. It's handed to a new " +
                "member face to face: the leader taps Invite someone, the member taps Join a team, " +
                "and the two phones pair.\n\n" +
                "To send to a team, turn on Encrypted and tap it to choose the team. To receive, " +
                "just turn on Encrypted: files for any team you're in open by themselves.\n\n" +
                "Anyone holding the key is in the team. To remove someone, make a new team and " +
                "invite everyone else."
    }
}
