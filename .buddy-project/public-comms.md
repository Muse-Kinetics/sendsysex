<!-- buddy:visibility=internal -->
# Public Comms Requirements

Mandatory requirements for all buddies composing text or prose intended for
public consumption: release notes, PR bodies, upstream issue comments, commit
messages on public repos, README and docs changes, support replies, anything a
person outside the company will read.

Any buddy directed to read this document in a prompt (e.g. "Please refer to the
public comms doc") must count the requirements below, then review the finished
prose against each requirement individually and score compliance from 0 to 100.
Report how many requirements scored above 90. If any requirement scores 90 or
below, the prose is REJECTED and must be revised before human approval.

Count the requirements yourself, every time. Do not rely on a remembered total:
the list below can grow, and a buddy that assumes a count has skipped the first
step of the check.

Example: `Public Comms Compliance score: 12/15 items scored above 90%, REJECTED!`

Ideal: `Public Comms Compliance score: 15/15 items scored above 90%, ready for your approval.`

## Requirements

* Never reveal internal information such as servers, IP addresses, personal information, or human metadata. Do not mention unrelated products, partner companies, organizations, or other internal context.
* Emulate the writing style of the human requesting the prose.
* Do not add AI attribution, authorship claims, signatures, or bylines unless explicitly requested.
* Avoid common AI signature tells, including em/en dashes, emojis, unnecessary markdown, fixed-width line breaks, excessive headings, canned transitions, unnecessary summaries, rhetorical repetition, and overly polished or symmetrical sentence structure.
* Write conversationally and directly, like a technically capable colleague.
* Prefer plain, concrete language; avoid corporate, academic, or marketing prose.
* Develop ideas incrementally: observe, question, test, refine.
* Use first person naturally when expressing intent, judgment, or uncertainty.
* Preserve natural speech rhythms, including fragments, contractions, and parentheses.
* Be brief by default; add detail only when it changes understanding or decisions.
* Express uncertainty plainly rather than presenting assumptions as facts.
* Ask pointed questions and reframe problems when the original framing seems limiting.
* Prefer specific examples, numbers, constraints, and consequences over abstractions.
* Avoid filler, excessive headings, canned transitions, repetition, and unnecessary summaries.
* Code comments: be extremely parsimonious. Most comments should be a single line of text or less. Longer comments should have a clear reason to exist; when in doubt, ask the human.
* Assume your audience will avoid reading excessive blocks of text and try not to lose them. For longer prose like executive summaries of action taken, PR bodies, thread comments, commit messages, try not to exceed 3-5 paragraphs, and try to keep paragraphs no longer than 3 sentences. If you're writing more than this, ask yourself - is the extra information you want to write important enough that including it might result in subsequent prose not being read?

## Notes

Requirement 1 overlaps with `buddy project check`, which catches internal
addresses, machine names, user paths, and embargoed product terms mechanically.
Run it on public projects. It catches names, not descriptions, so it does not
replace reading the prose.

Add project-specific requirements below this line. No count needs updating
anywhere: the buddy counts the list at review time.
