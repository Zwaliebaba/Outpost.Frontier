//! The command console that replaces the admin application's menus and
//! dialogs (decision O9): standard input read on its own thread, each
//! line parsed into a [`game_logic::admin::Command`] and handed to the
//! server loop through a channel. The console thread never touches the
//! game; the loop runs the commands between frames, as the dialogs ran
//! under `LockServer`.

use std::io::BufRead;
use std::sync::mpsc::{self, Receiver};

use game_logic::admin::Command;

/// One line of the console.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Input {
    /// An admin command.
    Command(Command),
    /// The command list.
    Help,
    /// Shut the server down (`COutpostServerApp::BeginShutdown`).
    Quit,
    /// A line that is not a command, with the reason.
    Invalid(String),
}

/// The command list [`Input::Help`] prints.
pub const HELP: &str = "\
Commands:
  status                              the server's counters and state
  players                             the players with a character
  accounts                            the accounts and their characters
  account create <name> [password]    a new account
  account delete <name>               delete an account and its files
  account admin <name> on|off         the right to universe chat
  character delete <name>             delete a character
  ban <account> | unban <account>     ban (disconnects the account's client)
  kick <account> | kick #<client>     disconnect a client
  say <text>                          universe chat
  chat system <nickname> <text>       chat to the players in a system
  chat player <character> <text>      private chat to a character
  news [set <text>]                   show or set the message of the day
  help                                this list
  quit                                save the players and stop
Names with spaces go in double quotes.";

/// The next word of `line` (a quoted run keeps its spaces) and the rest.
fn word(line: &str) -> Option<(String, &str)> {
    let line = line.trim_start();
    if line.is_empty() {
        return None;
    }
    if let Some(rest) = line.strip_prefix('"') {
        let end = rest.find('"')?;
        return Some((rest[..end].to_owned(), &rest[end + 1..]));
    }
    let end = line.find(char::is_whitespace).unwrap_or(line.len());
    Some((line[..end].to_owned(), &line[end..]))
}

fn name(rest: &str, what: &str) -> Result<(String, String), String> {
    let (name, rest) = word(rest).ok_or_else(|| format!("{what} expected"))?;
    Ok((name, rest.trim().to_owned()))
}

fn text(rest: &str) -> Result<String, String> {
    let text = rest.trim();
    if text.is_empty() {
        Err("text expected".into())
    } else {
        Ok(text.to_owned())
    }
}

fn on_off(word: &str) -> Result<bool, String> {
    match word {
        "on" | "yes" | "true" => Ok(true),
        "off" | "no" | "false" => Ok(false),
        _ => Err("on or off expected".into()),
    }
}

fn parse_command(line: &str) -> Result<Input, String> {
    let Some((verb, rest)) = word(line) else {
        return Err(String::new());
    };
    let command = match verb.to_ascii_lowercase().as_str() {
        "help" | "?" => return Ok(Input::Help),
        "quit" | "exit" | "shutdown" => return Ok(Input::Quit),
        "status" => Command::Status,
        "players" => Command::Players,
        "accounts" => Command::Accounts,
        "account" => {
            let (sub, rest) = word(rest).ok_or("create, delete or admin expected")?;
            match sub.as_str() {
                "create" => {
                    let (name, rest) = name(rest, "an account name")?;
                    let password = word(&rest).map(|(p, _)| p).unwrap_or_default();
                    Command::CreateAccount { name, password }
                }
                "delete" => Command::DeleteAccount {
                    name: name(rest, "an account name")?.0,
                },
                "admin" => {
                    let (name, rest) = name(rest, "an account name")?;
                    Command::SetAdmin {
                        name,
                        admin: on_off(rest.trim())?,
                    }
                }
                _ => return Err("create, delete or admin expected".into()),
            }
        }
        "character" => {
            let (sub, rest) = word(rest).ok_or("delete expected")?;
            if sub != "delete" {
                return Err("delete expected".into());
            }
            Command::DeleteCharacter {
                name: name(rest, "a character name")?.0,
            }
        }
        "ban" | "unban" => Command::Ban {
            name: name(rest, "an account name")?.0,
            ban: verb.eq_ignore_ascii_case("ban"),
        },
        "kick" => {
            let (target, _) = name(rest, "an account name or #client")?;
            match target.strip_prefix('#') {
                Some(id) => Command::KickClient {
                    client: id.parse().map_err(|_| "a client number expected")?,
                },
                None => Command::Kick { name: target },
            }
        }
        "say" => Command::ChatUniverse { text: text(rest)? },
        "chat" => {
            let (sub, rest) = word(rest).ok_or("universe, system or player expected")?;
            match sub.as_str() {
                "universe" => Command::ChatUniverse { text: text(rest)? },
                "system" => {
                    let (system, rest) = name(rest, "a system nickname")?;
                    Command::ChatSystem {
                        system,
                        text: text(&rest)?,
                    }
                }
                "player" => {
                    let (character, rest) = name(rest, "a character name")?;
                    Command::ChatPlayer {
                        character,
                        text: text(&rest)?,
                    }
                }
                _ => return Err("universe, system or player expected".into()),
            }
        }
        "news" => match word(rest) {
            None => Command::News,
            Some((sub, rest)) if sub == "set" => Command::SetNews {
                text: rest.trim().to_owned(),
            },
            Some(_) => return Err("news or news set <text> expected".into()),
        },
        _ => return Err(format!("unknown command {verb}; type help")),
    };
    Ok(Input::Command(command))
}

/// One console line; `None` for an empty one.
#[must_use]
pub fn parse(line: &str) -> Option<Input> {
    if line.trim().is_empty() {
        return None;
    }
    Some(parse_command(line).unwrap_or_else(Input::Invalid))
}

/// Reads `input` on a new thread; each parsed line arrives on the channel.
/// The thread ends at the end of the input or when the receiver is gone;
/// the end of the input does not stop the server.
///
/// # Errors
///
/// The thread cannot be started.
pub fn spawn(input: impl BufRead + Send + 'static) -> std::io::Result<Receiver<Input>> {
    let (tx, rx) = mpsc::channel();
    std::thread::Builder::new()
        .name("console".to_owned())
        .spawn(move || {
            for line in input.lines() {
                let Ok(line) = line else {
                    break;
                };
                if let Some(parsed) = parse(&line)
                    && tx.send(parsed).is_err()
                {
                    break;
                }
            }
        })?;
    Ok(rx)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn command(line: &str) -> Command {
        match parse(line) {
            Some(Input::Command(c)) => c,
            other => panic!("{line}: {other:?}"),
        }
    }

    #[test]
    fn commands_parse() {
        assert_eq!(command("status"), Command::Status);
        assert_eq!(command("  PLAYERS "), Command::Players);
        assert_eq!(command("accounts"), Command::Accounts);
        assert_eq!(
            command("account create pilot secret"),
            Command::CreateAccount {
                name: "pilot".into(),
                password: "secret".into()
            }
        );
        assert_eq!(
            command("account create pilot"),
            Command::CreateAccount {
                name: "pilot".into(),
                password: String::new()
            }
        );
        assert_eq!(
            command("account delete pilot"),
            Command::DeleteAccount {
                name: "pilot".into()
            }
        );
        assert_eq!(
            command("account admin pilot on"),
            Command::SetAdmin {
                name: "pilot".into(),
                admin: true
            }
        );
        assert_eq!(
            command("character delete \"Ace Rimmer\""),
            Command::DeleteCharacter {
                name: "Ace Rimmer".into()
            }
        );
        assert_eq!(
            command("ban pilot"),
            Command::Ban {
                name: "pilot".into(),
                ban: true
            }
        );
        assert_eq!(
            command("unban pilot"),
            Command::Ban {
                name: "pilot".into(),
                ban: false
            }
        );
        assert_eq!(
            command("kick pilot"),
            Command::Kick {
                name: "pilot".into()
            }
        );
        assert_eq!(command("kick #3"), Command::KickClient { client: 3 });
        assert_eq!(
            command("say hello  all"),
            Command::ChatUniverse {
                text: "hello  all".into()
            }
        );
        assert_eq!(
            command("chat system li01 docking closed"),
            Command::ChatSystem {
                system: "li01".into(),
                text: "docking closed".into()
            }
        );
        assert_eq!(
            command("chat player \"Ace Rimmer\" hi"),
            Command::ChatPlayer {
                character: "Ace Rimmer".into(),
                text: "hi".into()
            }
        );
        assert_eq!(command("news"), Command::News);
        assert_eq!(
            command("news set Welcome to Frontier"),
            Command::SetNews {
                text: "Welcome to Frontier".into()
            }
        );
    }

    #[test]
    fn other_lines() {
        assert_eq!(parse(""), None);
        assert_eq!(parse("help"), Some(Input::Help));
        assert_eq!(parse("quit"), Some(Input::Quit));
        for line in [
            "frobnicate",
            "kick",
            "kick #x",
            "say",
            "account admin pilot maybe",
            "character remove x",
            "chat player \"unterminated hi",
        ] {
            assert!(matches!(parse(line), Some(Input::Invalid(_))), "{line}");
        }
    }

    #[test]
    fn the_thread_hands_over_each_line() {
        let rx = spawn(std::io::Cursor::new(b"status\n\nbogus\nquit\n".to_vec())).unwrap();
        let got: Vec<Input> = rx.iter().collect();
        assert_eq!(got.len(), 3);
        assert_eq!(got[0], Input::Command(Command::Status));
        assert!(matches!(got[1], Input::Invalid(_)));
        assert_eq!(got[2], Input::Quit);
    }
}
