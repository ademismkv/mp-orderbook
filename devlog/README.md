# devlog

Dated entries. Each one: what you asked an AI tool for, what it produced, what you kept, what you changed and why, what it got wrong (especially about low-latency assumptions — AI tools default to "correct" idioms that are often not the fast idiom).

This folder is itself the differentiator, not a formality — see the reasoning in `04-PROJECTS/2026-07-17-matching-engine-orderbook.md` in the notes vault ("AI workflow" section). Nobody else applying for the same internships is shipping this publicly. Keep entries honest, including the times AI-suggested code was wrong or you had to fight it — that's the credible version, not a highlight reel.

## Entry template

```
## YYYY-MM-DD — [what you were building]

**Asked for:** [prompt / task given to the AI]
**Got:** [what it produced, briefly]
**Kept:** [what you kept as-is]
**Changed:** [what you rewrote and why — be specific: "it used std::mutex here, replaced with the single-writer design from ADR-1 because..."]
**Wrong:** [anything it got wrong, especially performance assumptions]
```
