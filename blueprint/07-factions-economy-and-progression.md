# 07 — Factions, economy and progression

Status: draft
Depends on: 00 (glossary), 01 (solars, bases, jump points), 02 (equipment,
hardpoint classes, hold), 11 (authority, character, worth audit), 12 (record
kinds, census). Refers forward to 03, 05, 06, 08, 09, 10.

## 1. Purpose and player experience

This chapter covers the three slow systems that give flying and fighting a
purpose: who likes whom, what things cost, and how far the player has come.

**Factions.** Every ship and station the player meets belongs to one of a
few dozen factions: house militaries and police, corporations, outlaw
bands, guilds. Each has a view of every other and of the player. The
player sees it as the colour of a contact, as a list of bars in the
status screen, and as consequences: patrols that open fire, stations that
refuse docking, dealers that withhold their better wares. Killing a
faction's ships lowers the player's standing with it, working for it
raises it, and both spill over to its friends and enemies, so every
choice of target is a choice of sides. Money can buy standing back.

**Economy.** There is one currency. Bases sell and buy commodities at
fixed, authored prices that differ from place to place, so profit comes
from knowing the map. Equipment and ships cost the same everywhere but
are offered only in some places, to players of sufficient level and
standing. No price reacts to trade and no stock runs out.

**Progression.** The player's level is a function of **worth**: money plus
the value of ship and equipment. Level gates what dealers will sell and
how hard offered jobs are. In the campaign, level and the next scripted
mission are released by earning a set amount of worth between missions,
which pushes the player into trade and jobs before the story continues.

## 2. Concepts

Glossary terms (faction, reputation, attitude, good, commodity, market,
level, character, base, solar, loadout, hold) are used as defined. This
chapter adds:

| Concept | Definition |
| --- | --- |
| Faction matrix | For each ordered pair of factions, the reputation one holds towards the other. Authored; part of the saved game in single player. |
| Reputation record | Per ship, solar and player: a faction or none, standings, personal adjustments, a level, a display name. |
| Standing | A reputation stored in a record against one faction. For a player it is the player's reputation with that faction, −1 to 1. |
| Personal adjustment | A reputation offset one record keeps towards one individual object: "this ship is angry at that ship". |
| Regard | The computed reputation of one object towards another, −1 to 1, from which the attitude is read. |
| Affiliation | The faction a record belongs to. NPCs are created with one. A player acquires one only by very high standing. |
| Empathy | Per faction, the share of a standing change that each other faction applies to its own standing with the player. Negative for enemies. |
| Event rate | Per faction, the standing change for each kind of event: its ship destroyed, a job for it completed, failed, abandoned. |
| Market row | One line of a base's market: a good and its conditions at that base. |
| Nominal price | The price in the good's own record. A base without a row for the good uses it. |
| Used rate | 0.3, the fraction of price paid for equipment the player already owned. |
| Worth | Money plus the valued ship and equipment. |
| Campaign stage | A counter, 0 to 42, of how far the campaign has advanced. |
| Contraband | A commodity that some lawful faction's patrols scan for. |

```text
faction ──matrix──► faction            faction ──event rates, empathy──► standing changes
   ▲ affiliation                       faction ──properties──► legality, scanned goods,
reputation record (ship, solar, player)                        NPC ships, names (ch. 06)
   ├─ standings[faction]      (the player's: reputation with each faction)
   └─ personal adjustments[object]

good ──kind──► commodity | equipment | hull | package ──► hull + fitted items
base ──market rows──► good: min level, min standing, stocked?, direction, price multiplier
solar ──authored faction──► owner of the base it carries ──► "standing with this base"

money + value(hull) + value(equipment) = worth ──level table──► level
```

## 3. How it works

### 3.1 Factions and the matrix

A faction record holds a nickname, three text identifiers and one
reputation towards every faction. The original ships 55 factions and a
full 55 × 55 matrix that uses only five values: −0.65 (enemies), −0.3
(dislike), 0, 0.65 (friends) and 0.91 (close allies). About 15% of the
pairs are enemies and 18% friends. A faction's reputation towards itself
counts as 1.

The matrix decides how NPCs treat each other. It is loaded with a new game
and written into the single-player save, and a service exists to change a
pair at run time, so the campaign can redraw alliances.

A separate property record per faction (chapter 06 uses most of it) adds
legality, which jump points its ships prefer (regulated, unregulated,
any), the NPC ships it fields, and the goods its patrols scan for. Of 55
factions, 30 are lawful and 25 unlawful. Nine campaign-only factions are
barred from ever becoming a player's affiliation.

### 3.2 Regard: how one object views another

Every reputation question in the game ("should this patrol shoot?", "may I
dock?") reduces to one function, evaluated on the authority:

```text
regard(A towards B) =
    clamp(-1, 1,  base + standing + personal),  rounded to hundredths
  base     = 0 if B belongs to a player, or A or B has no affiliation
             1 if A and B have the same affiliation
             matrix[A's faction][B's faction] otherwise
  standing = the value B's record stores against A's faction (0 if none)
  personal = the adjustment A's record stores against B itself (0 if none)
```

Two cases cover almost everything:

- **NPC towards NPC:** the matrix value, plus any personal adjustment.
- **NPC towards a player:** the player's standing with the NPC's faction,
  plus any personal adjustment. The matrix plays no part, so a player is
  judged by deeds alone.

Setting a regard or a standing stores the difference to the computed base,
so "set to x" always means that the final value becomes x.

A presentation connected over the network does not compute regard. It
shows the value the authority last sent for each object.

### 3.3 Attitude thresholds

| Regard | Attitude | Meaning |
| --- | --- | --- |
| ≤ −0.6 | Hostile | Attacked on sight, shown as hostile, docking refused. |
| between | Neutral | Left alone. |
| ≥ 0.6 | Friendly | Shown as friendly; friends take the player's side in a fight. |

The same two numbers are used everywhere: targeting, formation and fire
control in the AI, the dock queue, the contact list, the target display
and the map. The matrix values ±0.65 sit just outside the thresholds, and
−0.3 is a warning step that changes nothing by itself.

### 3.4 What changes a standing

Standing changes are applied by content controllers on the authority,
always through one routine:

```text
change(player, faction F, delta, with_empathy):
    standing[F] = clamp(-0.9, 0.9, standing[F] + delta)
    if with_empathy:
        for every other faction G:
            standing[G] = clamp(-0.9, 0.9, standing[G] + delta * empathy[F][G])
```

| Event | Change with the faction concerned | Empathy |
| --- | --- | --- |
| Player destroys one of its ships | Event rate "destruction": −0.03 for most factions, down to −0.006 for a few | yes |
| Player shoots a cargo pod off one class of large ship | −0.1 | yes |
| Generated mission completed for it | Rate "success": 0.14 for half the factions, 0.028 to 0.13 for the rest | yes |
| Generated mission failed | −0.045 | yes |
| Generated mission abandoned | −0.0675 | no |
| Bribe paid on a base | Raises the standing to exactly 0.6 (section 3.15) | yes |
| Scripted mission action | Sets a standing outright (chapter 08) | no |

Empathy is authored per ordered pair. About 70% of the 2,970 entries are
zero; the rest lie between −0.45 and 0.4, with a few at 1.0 for factions
that always move together. Because the product of two negatives is
positive, killing a faction's ships *raises* the player with that
faction's enemies.

**Worked example.** The player destroys a patrol ship of a house police
faction (rate −0.03). Its house military has empathy 0.4 with it, an
outlaw faction −0.45. Results: police −0.03, military −0.012, outlaws
+0.0135. Starting from neutral 0, twenty such kills make the police
hostile (−0.6), leave the military at −0.24 and lift the outlaws to 0.27.
Climbing from 0 to friendly with a typical faction takes five completed
jobs at 0.14.

Event-driven changes stop at ±0.9. Authored values may exceed that: the
new-character templates start the player at 0.91 with one or two factions.

**Affiliation of a player.** After every change the authority checks the
player's record. If the standing with the current affiliation is still
above 0.65, nothing happens. Otherwise the player takes the faction with
the highest standing strictly above 0.9, or none. Since events stop at
0.9, a player keeps the authored starting affiliation until the standing
drops to 0.65, and cannot gain a new one by play (inferred from the two
constants). In multiplayer a change of affiliation is announced to the
player. Affiliation does not change how NPCs regard the player, because
the base term is zero for players.

### 3.5 Personal hostility: provocation and taking sides

Shooting at a ship does not change a standing. It changes the victim's
personal adjustment, so that one careless shot does not start a war with
a whole faction, and a deliberate attack turns only the ships present.

Provocation is tracked per attacker and victim, only when the attacker is
a player, neither side is already hostile, and the victim's regard is
below 1:

| State | Event | Next state |
| --- | --- | --- |
| Calm, score 0 | A hit adds 50 × (damage as a fraction of the victim's hit points). Continued fire adds 10 per second; a pause of more than a second restarts that clock. | Calm |
| Calm | Score above 25 | Warned: one warning event, which the chatter system voices (chapter 06) |
| Warned | Score reaches 50 | Provoked: the victim and every member of its formation set their regard of the attacker to −1 |
| Any | A hit by one of two special damage causes | Provoked at once |

A content heartbeat then spreads fights to bystanders. For each player it
takes up to 32 ships and solars within 5,000 units and looks at triples
A, B, C in which A is friendly to B (≥ 0.6), C is hostile to B (≤ −0.6)
and B and C are within 5,000 units of each other. A then takes a side by
setting a personal regard of −0.6, and an "NPC turned hostile" event is
sent:

- B has no affiliation (normally a player) and A likes C too. If C's
  hostility equals B's standing with C's faction, C is a known enemy and
  A turns on C. If not, C holds a personal grudge because B provoked it,
  and A turns on B.
- B has an affiliation: A turns on C, as when a patrol aids a freighter
  (the reconstructed test in this branch is doubtful, open question 8).

Personal adjustments live in the reputation record of the object that
holds them and go when it does (inferred). Only kills leave a lasting
mark.

### 3.6 What reputation affects

| Consumer | Rule |
| --- | --- |
| NPC targeting, fire control, formations (ch. 05) | A contact is a target when regard ≤ −0.6. |
| Docking (ch. 04) | A base or ship whose regard of the requester is ≤ −0.6 refuses, with a spoken notice. Some solar classes are exempt. |
| Dealers | A market row may demand a minimum standing with the faction that owns the base (section 3.10). |
| Base inhabitants (ch. 08) | A character sells knowledge at standing ≥ 0, 0.2, 0.4 or 0.6 with its faction, depending on the item; rumours use the same four steps; a bribe is offered only below 0.6. |
| Ambient population (ch. 06) | Which formations spawn near a player takes the standing with their faction into account. |
| Cargo scans (section 3.13) | Only ships whose regard of the target lies between −0.6 and an upper bound scan it. |
| Interface (ch. 09) | Contact and target colour in three bands, map symbols, the status screen's list of standings. |

### 3.7 Who owns a solar

Chapter 01 left open where stations get their faction. It happens in
two steps:

1. When the universe loads, every placed solar gets a reputation record
   keyed by its nickname, with its name, information card and a level
   taken from the solar archetype. No affiliation is applied at this
   point: the loader looks for a different key than the one the system
   definitions use, and the faction table is not loaded yet in any case
   (the second reason per design notes).
2. When a system becomes active on the authority (a player enters), the
   content layer reads that system's definition again. For every solar
   record that names a faction it finds the solar's reputation record and
   sets the affiliation, and it submits the solar's pilot and behaviour
   (chapter 05). When the system goes idle it releases these handles.

Of 3,578 placed objects, 2,124 name a faction. The dealer interface
reads the same authored entry directly: it finds the solar that docks to
the current base and looks up the player's standing with the faction
named there. Solars created by missions get their faction from the
mission definition.

For a builder: a solar's owner is authored on the solar, and the base it
carries inherits that owner.

### 3.8 Goods

| Kind | What it is | Stackable | Notes |
| --- | --- | --- | --- |
| Commodity | Trade cargo; refers to a cargo archetype (volume, loot appearance) | yes | 40 defined, 38 traded; all have volume 1. |
| Equipment | Refers to an equipment archetype (chapter 02) | ammunition, mines, countermeasures and repair consumables yes; the rest no | May include free ammunition with the purchase. |
| Ship hull | Refers to a ship archetype and carries its price | no | 32 defined. |
| Ship package | A hull good plus a list of fitted items by hardpoint | no | This is what a ship dealer offers. |

A commodity also carries four price thresholds, as multiples of its
nominal price. The interface uses them to mark a price as good, average or
bad for buying and for selling, and to list a base's best buys and best
sales on its information card. They do not change any price.

### 3.9 Markets and prices

A market row holds: good, minimum level, minimum standing, a stock range,
a direction (0 = the base sells and buys, 1 = the base only buys; a third
value, sells only, exists in code and is unused) and a price multiplier.

```text
price at base = truncate(nominal price × multiplier)     fixed at load
```

- **The same price applies in both directions.** A commodity bought and
  sold back at once costs nothing. The only spread is between bases.
- **Stock is a flag, not a quantity.** A base sells a good only if its row
  has a non-zero minimum and a positive maximum stock; the amount is then
  unlimited. The authored numbers serve only as weights when NPC cargo is
  generated (chapter 06).
- **A good without a row** at a base is not sold there but is bought at
  its nominal price.
- A per-base random price variance exists (each price scaled by a random
  factor around 1). No base sets it; prices are deterministic.
- Nothing responds to trade. There is no supply, demand or time.

Selling to a base:

| Item | Price paid |
| --- | --- |
| Commodity | Price at base. |
| Stackable equipment | Price at base. |
| Other equipment bought since the dealer screen was opened | Price at base × condition: a full refund, which lets the player try a fit. |
| Other equipment owned before that | round(price at base × 0.3), then × condition. |
| Hull, when buying another ship | truncate(price at base × mean part condition × hull condition) × 0.8. |

"Owned before" is decided against a snapshot of the loadout that the
dealer screen takes when it opens. Closing the screen ends the refund.

In shape: commodity nominal prices run from 2 to 1,200 (median 120).
178 bases trade commodities; a base lists a median of 9 and sells a
median of 3 of them. Multipliers range from near 0 to 100, with quartiles
around 0.7 and 3. Equipment and ships always have multiplier 1, so they
cost the same everywhere and differ only in where they are offered.

**Routes.** For each ordered pair of bases, take the best profit per unit
of hold among the goods the first base sells:

| Pairs | Median | Upper quartile | Best |
| --- | --- | --- | --- |
| Both bases in one system | 6 | 30 | 624 |
| Bases in different systems | 85 | 251 | 1,900 |

Trade inside a system pays almost nothing; profit grows with distance.
The best route for a typical commodity yields 350 to 1,200 per unit.
Worked example: an early ship with a hold of 20 on an 80-per-unit route
earns 1,600 a trip, about one beginner's job. The largest freighter, hold
275, on an 800-per-unit route earns 220,000, about the top job reward.
Hold size is the trader's progression, as guns are the fighter's.

### 3.10 What restricts a purchase

The dealer interface checks:

1. The base must sell the good (stocked row).
2. **Money:** count ≤ money ÷ price, including any bundled ammunition.
3. **Level**, in single player only: player level ≥ the row's minimum.
4. **Standing:** the player's standing with the base's owner ≥ the row's
   minimum. A row without a requirement carries −1.
5. **Fit:** for mounted equipment the ship needs a hardpoint whose class
   accepts the item (chapter 02), and a free one; such items are bought
   one at a time.
6. **Hold:** count ≤ free volume ÷ item volume.
7. **Carry limits:** repair consumables are capped per ship archetype.

Mission cargo and the engine cannot be sold.

Equipment rows step through levels 0, 2, 6, 10, 16, 22, 26 and 30. About a
quarter of them also ask for a standing between −0.2 and 0.75. Ship rows
ask for levels 0 to 32 and never for standing.

### 3.11 Buying a ship

A ship dealer lists packages. 59 bases have one; 46 of them offer one
ship and 13 offer three. Every ship dealer also has a buy-only row for
each of the other packages.

Choosing a package opens an exchange between the old ship and the new
one. The player moves equipment across: own items go onto the new hull
where a hardpoint accepts them, package items can be kept or left.

```text
cost = new hull price
     − trade-in of the old hull (section 3.9)
     + package items kept
     − own items left behind (at the prices of section 3.9)
```

The assignment of the last two terms is inferred from the shape of the
formula in the interface. The purchase is refused while cost exceeds
money, and the screen says by how much. Hull prices run from 6,000 to
658,000.

### 3.12 Repairs

| Item | Cost |
| --- | --- |
| Hull | (1 − condition) × hull hit points × base repair rate |
| Part | (1 − condition) × part hit points × base repair rate |
| Equipment | truncate(price at base × 0.3) × (1 − condition) × count |

The base repair rate is a per-base field with default 0.33 that no base
overrides. Any damaged entry costs at least 1.

### 3.13 Contraband and cargo scans

No good is flagged illegal. A commodity is contraband because a lawful
faction lists it under "scan for", with a severity of 0, 1 or 2. Nine
lawful factions list one or two commodities, the same two throughout.
Sixteen unlawful factions list 6 to 19 goods each: for them the same
mechanism is piracy.

| State | Event | Next state |
| --- | --- | --- |
| Patrolling | A ship within 2,500 units is not yet scanned, the scanner's regard of it is at least −0.6 and at most 0.6 (for a player target: at most 1.0 if the scanner is lawful, 0.2 if unlawful), and a roll succeeds (chance 0.3 for every faction that sets it) | Approaching |
| Approaching | Within 500 units | Announcing (lawful factions announce the scan by radio, unlawful ones do not) |
| Announcing | Line spoken | Scanning |
| Scanning | No listed good in the hold | Patrolling |
| Scanning | Listed good found (highest severity, then largest volume); the target is told to drop it | Waiting, 20 seconds |
| Scanning | The good is a player's mission cargo | Hostile |
| Waiting | Cargo jettisoned | Collecting: the scanner tractors it in; anyone else who takes it becomes its enemy |
| Waiting | Time runs out | Hostile: the scanner's formation sets its regard of the target to −1 |

An NPC target decides through its pilot (chapter 05). The scanner needs a
scanner with cargo range above 500 units. Each contraband commodity is
sold at about a dozen bases and bought elsewhere at up to 2 and 4.75
times its nominal price, so smuggling is ordinary trade with a 30% chance
per patrol met of losing the cargo or fighting for it.

### 3.14 Worth and level

```text
worth = money
      + 0.8 × nominal hull price
      + Σ stackable items: count × nominal price
      + Σ other equipment: truncate(condition × nominal price) × count × 0.3
```

The 0.8 and 0.3 are the resale rates of section 3.9: worth is roughly
what the player could turn into money. A second valuation with the same
rates, at the current base's prices, backs the multiplayer audit of
chapter 11.

A table maps worth to level: 38 authored points, from 11,000 for level 1
to about 2.4 million for level 38, each step 9% to 25% above the last
(level 10 at 59,000, level 20 at 211,000, level 30 at 842,000). The level
is recomputed whenever money changes. The player's status screen shows
the worth still needed for the next level.

- **Multiplayer:** level is the table value of worth and follows it both
  ways. The table is extended in steps of 12.5% up to the money cap. The
  level is stored in the reputation record, so other players can see it
  (inferred).
- **Single player, during the campaign:** the level rises by one at 18
  fixed campaign stages and does not depend on worth. Between missions
  the campaign waits. On finishing a mission it records "worth now plus a
  threshold" and starts the next mission when worth reaches that figure
  and no other mission is running. The thresholds are 3,500, 5,200, 7,000,
  10,000 and 16,000 after the first five missions, 36,000 and 30,000 after
  two later ones, and zero elsewhere, where missions follow directly.
- **Single player, after the campaign (stage 42):** level follows the
  table, upwards only.

What level does: it gates market rows in single player (section 3.10),
feeds job difficulty and with it job pay (chapter 08), and is shown beside
names. Kills, missions flown, bases and systems visited and total money
earned are counted and displayed but unlock nothing.

Progression is also geographic. The new-game data locks 27 jump points
for the player and 36 for NPC traffic; the campaign unlocks them (chapters
01 and 08).

### 3.15 Money

Money is one integer, clamped to 0 … 999,999,999 on every change. A new
single-player character starts with 500 and a multiplayer character with
2,000.

| Sources | Sinks |
| --- | --- |
| Generated mission pay: a table from difficulty to money, 1,800 at the bottom to 247,000 at the top (chapter 08) | Ships (6,000 to 658,000) |
| Campaign payments (chapter 08) | Equipment (10 to 367,000; guns about double per hardpoint class, chapter 12) |
| Commodity trade | Ammunition, mines, countermeasures, repair consumables |
| Selling loot: dropped NPC cargo and equipment, mined commodities (chapters 03, 06) | Repairs |
| Selling used equipment at 0.3, trading in a hull at 0.8 | Bribes |
| Gifts and trades from other players (chapter 11) | Losses on resale; cargo lost on death in multiplayer (chapter 11) |

**Bribes.** A base character may offer to repair the player's standing
with a faction, when it is below 0.6:

```text
shortfall = 0.6 − standing
price     = 10,000 × shortfall × 10, rounded to two or three significant digits
effect    = change(player, faction, +shortfall, with empathy)
```

From neutral that is 60,000; from −0.65 it is 125,000. The offer text
names up to three of the faction's enemies, taken from its most negative
empathy entries, because paying also lowers the player with them.

### 3.16 Authority and presentation

| Authority | Presentation |
| --- | --- |
| Owns the faction matrix, all reputation records, standings, personal adjustments; computes regard; applies every change; sends changed standings and affiliations | Shows the regard it was sent; colours contacts |
| Loads markets; answers price queries; holds money and inventory; raises "good bought" and "good sold" events for content; recomputes level on every change of worth | Builds the dealer lists, computes each price, checks every restriction of section 3.10 and requests the resulting change of money and items |
| In multiplayer, audits worth on leaving a base (chapter 11) | — |

The purchase rules live on the presentation side and the authority only
audits the outcome. This is a trait of the original that a builder should
not copy (section 6).

## 4. Data that defines it

| Record | Count | Principal fields | Typical values |
| --- | --- | --- | --- |
| Faction | 55 | nickname; name, short name and information card text identifiers; one reputation per faction | −0.65, −0.3, 0, 0.65, 0.91 |
| Reputation effects | 55 | faction; rate per event kind (destruction, job success, failure, abandonment); one empathy rate per other faction | see 3.4 |
| Faction properties | 55 | legality; jump preference; NPC ship list; scan-for list (good, severity); scan chance; announce flag; names, voices, formations (chapter 06) | 30 lawful, 25 unlawful |
| Good | 855 | nickname, kind, nominal price, archetype reference, stackable flag, icon; commodity: four price thresholds; package: hull, fitted items | commodity 2 to 1,200 |
| Market | 413 base records, 14,694 rows | base; per row: good, minimum level, minimum standing, stock range, direction, price multiplier | see below |
| Per-faction trade goods | 48 | faction; the commodities its freighters may carry | 0 to 38 goods, 626 rows |
| Level table | 1 | points (worth, level) | 38 points |
| Difficulty-to-money table | 1 | points (difficulty, pay) | 23 points, 1,800 to 247,065 |
| Base roster (chapter 08) | 194 | per character: bribes (faction, cost factor, text) | cost factor 10,000 throughout |
| New-character templates | 2 | money, starting ship package, one standing per faction | 500 and 2,000 |

Example market rows (placeholders, real proportions):

| Base | Good | Min level | Min standing | Stock | Direction | Multiplier |
| --- | --- | --- | --- | --- | --- | --- |
| `example_mining_base` | `example_ore` | 0 | −1 | 150–500 | sells and buys | 0.5 |
| `example_capital_base` | `example_ore` | 0 | −1 | 0–0 | buys only | 1.35 |
| `example_capital_base` | `example_gun_class4` | 10 | 0 | 10–10 | sells and buys | 1 |

## 5. Connections

**Needs.** 01: solars, bases, jump points, system activation. 02: hull
and equipment archetypes, hardpoint classes, hold volume, condition. 11:
the character file (money, level, standings), the worth audit, player
trade. 12: record files, nickname hashing (faction nicknames use a 16-bit
hash there).

**Gives.** 03: who counts as an enemy, the provocation events fed by
damage reports. 04: the docking refusal. 05: regard as the input of
targeting; scan responses. 06: faction properties for spawning, market
rows for NPC cargo, the hostility events that chatter voices. 08: event
rates for mission outcomes, job pay, the campaign's worth thresholds,
bribes and knowledge thresholds. 09: attitude bands for contact and
target display, standings for the status screen. 10: dealer screens, the
good-price markers, the base information card.

## 6. Design notes for the builder

**Essential.**

- One regard function with two fixed thresholds that everything reads.
  This is why faction behaviour is consistent everywhere.
- Standing changes that propagate. Without empathy the factions are
  independent meters; with it they form blocs and every kill is a
  political act.
- The split between standings (lasting, per faction, changed by kills and
  jobs) and personal adjustments (temporary, per object, changed by
  hits). Without it a stray shot is either meaningless or catastrophic.
- Prices that vary by place and not by time, with profit that grows with
  distance. The universe map becomes the trading game.
- Worth as the single measure of progress. Trading, fighting and jobs all
  feed it, so no play style is locked out of better ships.

**Feel.** The five-step matrix; ±0.9 as the limit of earned standing;
the resale rates (generous on hulls, harsh on guns, so changing ship is
cheap and refitting is not); the 20 seconds to drop contraband; the
warning before provocation; the refund while the dealer screen is open.

**Accidents of the era.**

- The presentation computes purchases and the authority audits worth
  afterwards. Put the rules on the authority.
- Solars get their faction in a second pass, by a different module than
  the one that loads them. Apply the owner at load.
- Level is stored in two places with different rules for single and
  multiplayer, and the level gate on purchases is skipped in multiplayer.
- A 16-bit hash for faction nicknames (chapter 12).
- Stock ranges, price variance and the sells-only direction are remnants
  of a more dynamic economy that was not built (inferred).

**Simplifications.**

- Fewer factions work as well. Keep a lawful and an unlawful bloc per
  region, or kills lose their second edge.
- Dropping empathy costs the bloc politics. Dropping personal adjustments
  costs the warning shot and makes bystander behaviour hard.
- A dynamic economy is not required. If added, the authority must own
  stock.
- The campaign's worth thresholds can be replaced by any pacing gate, at
  the loss of the reason to trade and explore between missions.

## 7. Open questions

1. **Provocation causes.** Hits count for six of the damage-cause codes
   and two of them provoke at once. Which causes these are was not
   matched reliably against the damage model of chapter 03.
2. **The cargo-pod penalty.** The object class that loses 0.1 standing
   per destroyed cargo pod was read as a type code only; "large transport"
   is inferred.
3. **Docking exemptions.** Which solar classes skip the hostility test
   when docking (lane rings and jump points, inferred) was not confirmed.
4. **Ship exchange.** The exact roles of the two equipment terms in the
   ship purchase cost, and what happens to items that fit nowhere.
5. **Multiplayer jobs.** Job difficulty reads the campaign stage, which
   multiplayer never advances; how offers scale there was not traced
   (chapter 08).
6. **Run-time changes to the matrix.** The service exists and the matrix
   is saved; which scripted events use it was not traced.
7. **A "jump distance" field on commodities** (1 to 10) is loaded and can
   be queried by content; no consumer was found.
8. **Taking sides.** In the branch for an affiliated victim the
   reconstructed code lets A act only when its regard of C is exactly
   −0.6, which cannot be the intent. The heartbeat period was not read.
   Whether kills of mission-spawned ships change standing is also open.

## 8. Basis

Design documents: `design/assessment/02-simulation-core.md` (subsystem
table), `design/assessment/04-server-content.md` ("Economy is not
simulated", save format), `design/facts.md` F17 (solar factions at load),
`design/adr/0007-server-world.md`.

Data, tabulated with scripts: `outdir/data/initialworld.ini`,
`MISSIONS/empathy.ini`, `MISSIONS/faction_prop.ini`,
`EQUIPMENT/goods.ini`, `misc_good.ini`, `st_good.ini`, `weapon_good.ini`,
`market_commodities.ini`, `market_misc.ini`, `market_ships.ini`,
`commodities_per_faction.ini`, `select_equip.ini` (commodity volume),
`MISSIONS/ptough.ini`, `MISSIONS/rankdiff.ini`,
`RANDOMMISSIONS/diff2money.ini`, `MISSIONS/mbases.ini` (bribe rows),
`UNIVERSE/universe.ini` and the system files (faction entries),
`outdir/newplayer.fl`, `outdir/mpnewcharacter.fl`,
`CHARACTERS/newcharacter.ini`.

Code: `Common/common_reputation.cpp`, `common_vibe.cpp` (regard, clamp,
rounding, affiliation), `common_goodinfo.cpp`, `common_basedata.cpp`
(market rows, price), `common_cobjdb.cpp` (solar records at load),
`common_tradeoffer`, `include/common_goods.h` (transaction types);
`Content/CombatReputation.cpp`, `FactionProps.cpp` (event rates, empathy,
clamp), `BaseDB.cpp`, `BaseNpcRecords.cpp` (bribes, thresholds),
`NpcCreate.cpp`, `SystemInfo.cpp` (solar factions), `PatrolGuides.cpp`
(cargo scans), `Support.cpp`, `Story.cpp`, `PlayerState.cpp` (worth,
level, campaign thresholds), `DockQueues.cpp`, `RandomMissionDB.cpp`,
`RandomMissionGen.cpp` (money rounding), `Cargo.cpp`;
`Server/server_classes.cpp` (money cap, audit valuation),
`server_pub.cpp` (asset value, price query);
`OutpostFrontier/Client/client_base_market.cpp`, `nn_dealer.cpp`,
`nn_inventory.cpp`, `nn_ship_trader.cpp`, `nn_repair.cpp`,
`nn_help.cpp`, `nn_ui_pulse.cpp`, `hud_target.cpp`.
`Common/common_repairmanager.cpp` was read and found to concern NPC use
of repair consumables (chapter 05), not base repairs.

Inference: the statement that a player cannot gain an affiliation by
play; the roles of the terms in the ship purchase cost; the reading of
unused market fields as remnants; items marked "(inferred)" in the text.
Route statistics were computed from the market rows under the rule that a
base without a row buys at nominal price. Census counts not recomputed
here (855 goods, 413 market records, gun price progression) are those of
chapter 12.
