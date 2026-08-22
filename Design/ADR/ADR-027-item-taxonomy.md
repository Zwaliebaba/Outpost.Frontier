# ADR-027 — The Item Taxonomy: Stacks, Families, and Containers

**Status:** Accepted · 2026-08-22 (owner ruling: draft from the prints, assumptions flagged)
**Depends on:** ADR-024 (§2 the ores, §5b the wreck, §5c the Bay and the manual transfer, §5d the
icon rules, §6a fungibility as a design constraint, §6d the first commodity), ADR-012 (content is
hash-guarded JSON), ADR-014 (the engine/game seam), ADR-020 (surfaces; D-P6's one component)
**Closes:** the corpus's **standing item-taxonomy gap** — three prints cite an upstream document
this repository cannot follow ([D-P2](../ScreenPrints/cargo-tab.png),
[D-P4](../ScreenPrints/market-tab.png), [D-P6](../ScreenPrints/container-surface.png)), recorded
twice in [README.md](../README.md) so it would be paid rather than forgotten.
**Blocks nothing that is built.** E5 needs §1 and §2; the market and container phases need §3 and
§4 before their own ADRs can be written.

## Context

Ore is in the tree. `EconomyDef` parses three ores, `CargoStatus` and `BayStatus` carry counts of
them, and the hold arithmetic is litres. What is *not* in the tree is any statement that ore is a
**kind of thing** rather than the only thing — and three prints have been drawn against exactly
that statement.

They did not invent it. Each cites an upstream document by a numbering scheme this corpus does
not use (`08c`, `08c §10`), and each states its posture in an amber banner rather than pretending
the citation resolves. That was the right call at drawing time and the wrong state to build from:
**a plate that cites a document the repository does not have is a plate whose claims cannot be
checked.** This ADR makes those claims checkable by restating them here, as this corpus's own.

**It restates rather than references.** The upstream numbering is not this corpus's and the
document is not in it, so every rule below stands on either an existing ADR, a drawn print, or a
flagged assumption in §6. Nothing below should be read as reporting what the upstream document
says.

## Decision

### 1. An item is an id, and a holding is a triple

**`ItemTypeId` is a `u16` naming one item type**, allocated in content (`Economy.json`) and
hash-guarded like every other economy number (ADR-012 §D13). It is the *only* identity an item
has: there is no name on the wire, no category byte, no family byte. Everything else about an
item — its family, its category, its unit volume, its icon — is a property the content states and
both halves read from the same parsed table.

**Every holding, everywhere, is `(ItemTypeId, units, litres)`.** D-P2 fixed this as the tab's
whole vocabulary and it is promoted here to the rule: the hull manifest, the Bay's rows, the MOVE
chips, the fill preview, a wreck's contents, a market escrow's goods. **Litres are derived, never
authored per holding** — `units × unitVolumeLitres` from the content table — because a holding
that carried its own volume could disagree with the item's, and one of the two would be a lie
about the same stack.

**Stacks are whole.** A stack moves entire unless the destination cannot take it, and the only
split any surface computes is fill-to-capacity, by the same pure function the authority validates
with (D-P2 §3, and the parity rule ADR-014 §3 applies to every other verb).

**Ore's existing wire is a special case of this and stays as it is.** `CargoStatus` and
`BayStatus` carry three ore counts because three ores are what exists; they generalise to
`ItemTypeId` when a second family reaches the wire, which is E4's alloys. **No wire change is made
by this ADR** — it says what the fields mean, not what they are.

### 2. Families are a display grouping, and the panel never reshuffles

**A family is a content-declared grouping of item types** — ores, alloys, commodities today — and
it exists for one reason: D-P2's rule that *"families stack downward as they exist; rows join
their group; the panel never reshuffles."* A family is therefore **ordered**, and its order is
content's, not the client's.

**Fill order is ore-index order** *(owner ruling, 2026-08-22)*. When a transfer, a fill or a scoop
cannot take everything, items move in the order content declares them — Ferro-Chroma → Astracite →
Nebulite today, and each family's own declared order after that. **One ruling, three questions:**
this settles D-P2 §3's ore-order-on-a-fill, D-P6 §3's partial-scoop order, and ADR-024 §5d's
statement of the same question, which the ADR itself predicted would close together.

The argument is not that index order is fairest. It is that **value-density order needs a price
table, and there is no market**: a rule computed from prices would silently re-order itself the
day the market phase tunes one, so a player who learned the rule would find it had changed
without a patch note. Index order is stable, teachable, and already the order every screen lists
items in — so the rule learned on the cargo tab is the rule a wreck obeys. If a later phase wants
density, it is a new rule with a name, not this one drifting.

### 3. Fungible and instance — the split that makes a market possible

**Every item type is one or the other, declared in content, and today every item is fungible.**

- **Fungible**: two units of the type are interchangeable. They stack, they aggregate into one
  row, one number describes any quantity of them. Ores, alloys and commodities are all fungible.
- **Instance**: a unit carries its own state — quality, a blueprint's rolls, wear. Instances never
  stack and never aggregate.

**Fungibility is the market's admission rule**, not a hint. ADR-024 §6a stated it as a design
constraint — *"a Ferrocite Plate is a Ferrocite Plate or the future market has no order book"* —
and D-P4 enforces it: instance items never list on a book, and the surface they will need is a
contract screen that is deliberately not sketched. **A book quotes a type, and a type is only
quotable if any unit satisfies any order.**

**No instance item exists**, and none may be added without an ADR that also says which surface
trades it. That clause is the whole value of naming the split now: it costs nothing today and it
stops the first quality-varying item from arriving on a screen that assumed it could not.

### 4. Categories, and the growth rule

**Three flat categories — ORES · ALLOYS · COMMODITIES** (D-P4, and its plate says the question is
answered). Nine listable items do not need a tree, and a browser that opened folders to find
Plates would be ceremony.

**The growth rule is pre-authorised rather than deferred**: a category whose rows pass one screen
splits into a tree. Same shape as D-P2's five-column collapse — a stated threshold with a stated
consequence, so the first person to add a tenth alloy is following a rule rather than making one.

**Category is a content property of the item type**, so a re-categorisation is a content edit and
a hash bump, never a schema change.

### 5. Containers: one component, kinds that differ by chrome

**A container is a holding of items that is not a hull's hold and not a station Bay.** D-P6's
call is promoted here: **one component every kind instantiates** — an identity header (kind, claim
rule, lifetime), the `(item, units, litres)` stack list of §1, and **at most one verb**. Kinds
differ by header, chrome and that one verb; **never by layout**. So a new kind costs a header and
a verb, not a screen.

**Three kinds are grounded in this corpus and are normative:**

| Kind | Claim rule | Lifetime | Verb |
|---|---|---|---|
| **Wreck** | finders-keepers — nobody owns it (ADR-024 §5b) | 900 s scatter | SCOOP |
| **Market escrow** | the lister's, held at the station | until the order fills or is cancelled | none here — release is CANCEL on the market tab |
| **Deploying** | the owner's | until the assembly completes | none |

**Deploying exists because E3's accept found the invisible version** — `ApplyTransit` spawning
arrivals with empty holds, cargo lost with no surface on which to notice. A container kind whose
whole job is that in-between state is the fix made visible, and it is the reason this section
exists rather than waiting for the market.

**The "six kinds" the prints assert is not restated as six.** See §6.

### 6. Assumptions — flagged, because this ADR was drafted from prints rather than from a source

Every rule above stands on an existing ADR or a drawn plate. These do not, and are marked so the
first person with better information corrects a stated assumption rather than discovering an
invented one:

1. **`u16` for `ItemTypeId`.** Chosen for the reason `HullClass` is small and `AnchorId` is not:
   item types are authored content in the dozens-to-hundreds, and the wire already spends u16s on
   ids of that scale. Nothing measured it.
2. **Litres as the single capacity dimension.** The economy is litres today and mass never
   appears. A second dimension (mass, or a per-item slot cost) would change the fill arithmetic in
   §1 and every preview that shares it.
3. **Three prints assert "six container kinds"; this ADR names three.** The other three are not
   in this corpus in any form — not drawn, not cited by section, not implied by a mechanic — so
   naming them would be inventing them. **The six-kind claim is therefore not restated**, and
   D-P6's banner stands: the number came from upstream. When the remaining kinds land they cost a
   row in §5's table.
4. **Family is display grouping only.** Nothing in this corpus gives a family a mechanical
   consequence — no per-family cap, no per-family fee, no per-family refinery rule. If one ever
   does, family stops being presentational and this section is where that changes.
5. **Categories and families are the same partition today** (ores, alloys, commodities are both).
   They are separate concepts because D-P4's categories are a *market browser* structure and
   D-P2's families are a *Bay panel* structure, and the first item that wants to sit in one
   grouping but not the other is why they are not one field.

## Consequences

- **Three prints stop citing a document this repository does not have.** D-P2's "ore is the first
  item family", D-P4's admission rule and D-P6's one-component claim now rest on §1–§5 here. Their
  amber banners should be re-read against this ADR at their next version bump; the plates
  themselves do not change, which is why this costs no re-capture.
- **E5 builds against §1 and §2 rather than against ore.** The cargo tab's vocabulary is already
  the triple, so this is a rename in the reading rather than a change in the building.
- **The market ADR inherits §3 and §4** and owes only what it was always going to owe: the
  currency, the fee point and rate, order lifetime, and who seeds the first book.
- **No wire change, no schema bump, no content change.** This ADR states what existing fields mean
  and constrains what future ones may be. `EconomyDef`'s three ores and five alloys are unmoved,
  and the replay hash cannot notice it.
- **One rule is enforceable today and should be**: litres derived, never stored per holding (§1).
  The ore path already does this; a test asserting it is cheap and stops the second family
  arriving with its own volume field.

## Alternatives considered

- **Wait for the upstream document.** Most accurate, and it blocks E5's honesty plus two phases'
  ADRs on an artefact with no owner in this repository. Declined by owner ruling 2026-08-22.
- **Declare upstream out of scope and write only what E5 needs.** Smaller and safer, and it leaves
  D-P4's admission rule and D-P6's one-component claim ungrounded — which is the state this ADR
  exists to end.
- **`ItemTypeId` as a string key.** Readable in content and self-describing on the wire; rejected
  for the reason ADR-014 rejects every other string crossing the seam, and because a stack list is
  the one place a per-row string would be paid for repeatedly.
- **Family and category as one field.** Simpler by one field and true today; rejected because the
  two structures are drawn by two different prints for two different surfaces, and collapsing them
  would make the first divergence a schema change instead of a content edit.
