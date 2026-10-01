# typed_ai

A DuckDB extension that asks a model typed questions about rows: yes/no, pick one, or a score. Answers are
probabilities from the model's own odds, and spending is capped from the first query.

Two providers ship: [TypeSafe Jev](https://docs.typesafe.ai) and any OpenAI-compatible server that returns
`logprobs` (OpenAI, vLLM, llama.cpp, Ollama, OpenRouter).

## Contents

- [Quick start](#quick-start)
- [Walkthrough for a first session](#walkthrough-for-a-first-session)
- [Safety first](#safety-first)
- [Functions](#functions)
- [Cost controls](#cost-controls)
- [When a job stops partway](#when-a-job-stops-partway)
- [Profiles](#profiles)
- [Other settings](#other-settings)
- [Build and test](#build-and-test)

## Quick start

```sql
LOAD typed_ai;
CREATE SECRET jev (TYPE typed_ai, PROVIDER 'jev', API_KEY 'ts_...');

SELECT * FROM bot_answers b WHERE NOT typed_is(b, 'the answer is fully supported by the source');
SELECT question, typed_pick(question, 'what is the visitor asking about?', (SELECT list(name) FROM topics)) FROM bot_answers;
FROM typed_usage();
```

With no secret, the built-in `jev` profile reads `TYPESAFE_API_KEY` from the environment.

## Walkthrough for a first session

A museum runs a chatbot that answers visitors' questions from the exhibit texts. This session audits it: which
answers the source does not support, how sure we can be, what each question is about, and whether a free local
model would have caught the same mistakes. Outputs are from a real run on DuckDB v1.5.6 with Jev (it cost
$0.00007) and Ollama with `gemma2:2b`. Probabilities can differ a little between runs and model versions.

### 1. Start DuckDB and load the extension

A local build is not signed by DuckDB, so start the CLI with `-unsigned`. The key comes from `TYPESAFE_API_KEY`.

```text
$ duckdb -unsigned
D LOAD '/path/to/duckdb-typed-ai/build/release/extension/typed_ai/typed_ai.duckdb_extension';
D CREATE SECRET jev (TYPE typed_ai, PROVIDER 'jev', API_KEY getenv('TYPESAFE_API_KEY'));
```

**No API key yet?** Every step also runs free on your laptop with [Ollama](https://ollama.com). Run
`ollama pull gemma2:2b`, then use this secret instead of the Jev one:

```sql
CREATE SECRET local (TYPE typed_ai, PROVIDER 'openai', MODEL 'gemma2:2b',
  URL 'http://localhost:11434/v1/chat/completions', USD_PER_MTOK_IN 0, USD_PER_MTOK_OUT 0);
```

Expect weaker answers. In step 3 the local model flags only one of the three mistakes, where Jev flags all three:

```text
┌───────┬─────────────────────────────────────┐
│  id   │               answer                │
│ int32 │               varchar               │
├───────┼─────────────────────────────────────┤
│     6 │ It is around 150 million years old. │
└───────┴─────────────────────────────────────┘
```

The local model is good for trying the extension and for a free first pass (step 6); Jev is the one to trust for
the final call, because its odds are calibrated.

### 2. Load what the bot said

Each row holds the visitor's question, the exhibit text the bot should rely on, and the bot's answer. A second
table lists the topics the museum tracks.

```sql
CREATE TABLE bot_answers AS SELECT * FROM (VALUES
  (1, 'When was the observatory built?', 'The observatory opened in 1891 and was rebuilt after a fire in 1932.', 'It was built in 1891.'),
  (2, 'Who painted the ceiling?', 'The ceiling mural was painted by Ilse Marten between 1904 and 1906.', 'Ilse Marten painted it in 1910.'),
  (3, 'Can I take photos?', 'Photography without flash is allowed in all galleries.', 'Yes, flash photography is fine everywhere.'),
  (4, 'How heavy is the meteorite?', 'The Ashford meteorite weighs 412 kilograms.', 'It weighs about 412 kg.'),
  (5, 'Is the cafe open on Mondays?', 'The cafe is open Tuesday to Sunday.', 'No, the cafe is closed on Mondays.'),
  (6, 'How old is the dinosaur skeleton?', 'The skeleton is about 68 million years old.', 'It is around 150 million years old.')
) t(id, question, source, answer);

CREATE TABLE topics AS SELECT * FROM (VALUES ('history'), ('art'), ('visiting rules'), ('science'), ('food and drink')) t(name);
```

### 3. Find the answers the source does not support

Passing the whole row (`b`) sends all three columns as one JSON object, so the model compares the answer with
its source.

```sql
SELECT id, answer FROM bot_answers b WHERE NOT typed_is(b, 'the answer is fully supported by the source');
```

```text
┌───────┬────────────────────────────────────────────┐
│  id   │                   answer                   │
│ int32 │                  varchar                   │
├───────┼────────────────────────────────────────────┤
│     2 │ Ilse Marten painted it in 1910.            │
│     3 │ Yes, flash photography is fine everywhere. │
│     6 │ It is around 150 million years old.        │
└───────┴────────────────────────────────────────────┘
```

Rows 1, 4 and 5 are missing because their answers match the source; `NOT` keeps only the mistakes. Row 2 gets
the year wrong, row 3 the flash rule, row 6 the age.

### 4. Look at the odds before trusting a cut-off

`typed_is` cuts at 0.5 by default. The probabilities behind it show how clean the split is:

```sql
SELECT round(typed_prob(b, 'the answer is fully supported by the source'), 1) AS bucket, count(*) AS answers
FROM bot_answers b GROUP BY bucket ORDER BY bucket;
```

```text
┌────────┬─────────┐
│ bucket │ answers │
│ double │  int64  │
├────────┼─────────┤
│    0.0 │       3 │
│    0.8 │       1 │
│    0.9 │       1 │
│    1.0 │       1 │
└────────┴─────────┘
```

Nothing sits in the middle, so any cut-off between 0.1 and 0.8 gives the same result. On real data, pick the
cut-off where the buckets thin out and pass it as the third argument: `typed_is(b, '...', 0.7)`.

### 5. Take the options from a table

The topic list comes from `topics`, so adding a topic is an `INSERT`, not a query change.

```sql
SELECT id, question, typed_pick(question, 'what is the visitor asking about?', (SELECT list(name) FROM topics)) AS topic
FROM bot_answers;
```

```text
┌───────┬───────────────────────────────────┬────────────────┐
│  id   │             question              │     topic      │
│ int32 │              varchar              │    varchar     │
├───────┼───────────────────────────────────┼────────────────┤
│     1 │ When was the observatory built?   │ history        │
│     2 │ Who painted the ceiling?          │ art            │
│     3 │ Can I take photos?                │ visiting rules │
│     4 │ How heavy is the meteorite?       │ science        │
│     5 │ Is the cafe open on Mondays?      │ food and drink │
│     6 │ How old is the dinosaur skeleton? │ science        │
└───────┴───────────────────────────────────┴────────────────┘
```

Now the topics earn their place: which ones does the bot get wrong most?

```sql
SELECT typed_pick(question, 'what is the visitor asking about?', (SELECT list(name) FROM topics)) AS topic,
       count(*) AS asked,
       count(*) FILTER (WHERE NOT typed_is(b, 'the answer is fully supported by the source')) AS wrong
FROM bot_answers b GROUP BY topic ORDER BY wrong DESC, topic;
```

```text
┌────────────────┬───────┬───────┐
│     topic      │ asked │ wrong │
│    varchar     │ int64 │ int64 │
├────────────────┼───────┼───────┤
│ art            │     1 │     1 │
│ science        │     2 │     1 │
│ visiting rules │     1 │     1 │
│ food and drink │     1 │     0 │
│ history        │     1 │     0 │
└────────────────┴───────┴───────┘
```

Every judgment here was already made in steps 3 and 5, so this query is answered from memory and costs nothing.

### 6. Compare with a free local model

Save Jev's answers, switch the profile to a local model, ask again, and let SQL find the disagreements.

```sql
CREATE TABLE by_jev AS SELECT id, typed_prob(b, 'the answer is fully supported by the source') AS p FROM bot_answers b;

CREATE SECRET local (TYPE typed_ai, PROVIDER 'openai', MODEL 'gemma2:2b',
  URL 'http://localhost:11434/v1/chat/completions', USD_PER_MTOK_IN 0, USD_PER_MTOK_OUT 0);
SET typed_profile = 'local';
CREATE TABLE by_local AS SELECT id, typed_prob(b, 'the answer is fully supported by the source') AS p FROM bot_answers b;

SELECT j.id, round(j.p, 2) AS jev, round(l.p, 2) AS local_model, abs(j.p - l.p) > 0.5 AS disagree
FROM by_jev j JOIN by_local l USING (id) ORDER BY id;
```

```text
┌───────┬────────┬─────────────┬──────────┐
│  id   │  jev   │ local_model │ disagree │
│ int32 │ double │   double    │ boolean  │
├───────┼────────┼─────────────┼──────────┤
│     1 │   0.84 │         1.0 │ false    │
│     2 │   0.01 │        0.99 │ true     │
│     3 │   0.02 │         1.0 │ true     │
│     4 │   0.97 │         1.0 │ false    │
│     5 │   0.94 │         1.0 │ false    │
│     6 │   0.01 │         0.0 │ false    │
└───────┴────────┴─────────────┴──────────┘
```

The small local model missed the wrong year (row 2) and the flash rule (row 3). It costs nothing, so it suits a
first pass; Jev's odds are calibrated, so they suit the final call.

### 7. Check what you have spent

```sql
SET typed_profile = 'jev';
SELECT profile, requests, printf('$%.6f', spent_usd) AS spent FROM typed_usage();
```

```text
┌─────────┬──────────┬───────────┐
│ profile │ requests │   spent   │
│ varchar │  int64   │  varchar  │
├─────────┼──────────┼───────────┤
│ jev     │        8 │ $0.000067 │
└─────────┴──────────┴───────────┘
```

Asking the same question about the same row again is free: answers are saved in memory.

### 8. Price a big table before running it

With more than 1,000 rows the query is refused before anything is sent. A dry run prices it without calling
anything:

```sql
CREATE TABLE all_answers AS SELECT 'answer number ' || i AS answer FROM range(5000) r(i);
SET typed_dry_run = true;
SET typed_max_rows = 5000;
SELECT count(typed_prob(answer, 'the answer mentions a date')) FROM all_answers;
SELECT printf('$%.4f', dry_run_usd) AS would_cost FROM typed_usage();
SET typed_dry_run = false;
```

```text
┌────────────┐
│ would_cost │
│  varchar   │
├────────────┤
│ $0.0088    │
└────────────┘
```

If the price is fine, run it with `typed_dry_run` off. The budget (`typed_budget_usd`, default $1) still stops it
if the estimate was low.

## Safety first

- Row contents leave the machine and go to the provider, unless the profile points at a local server.
- Row text can try to steer the answer ("answer yes"). Rows are sent as data, apart from the question, which
  lowers the risk without removing it. Do not use `typed_is` as a security gate.
- A local model's odds are real probabilities, but only Jev's are calibrated. `typed_ask` reports which.

## Functions

| Function | Returns |
| --- | --- |
| `typed_is(input, statement [, threshold])` | `BOOLEAN`: probability of yes at or above the threshold (default 0.5) |
| `typed_prob(input, statement)` | `DOUBLE`: probability of yes, 0 to 1 |
| `typed_pick(input, question, options)` | `VARCHAR`: the option the model rates highest; options can come from a table |
| `typed_score(input, question, levels)` | `DOUBLE`: probability-weighted level, 0 to n-1 |
| `typed_ask(input, question_json)` | `JSON`: the full answer with probabilities, confidence, model and whether it is calibrated |
| `typed_usage()` | one row: spend, budget, requests, retries, cache, breaker, and the last query's counts |

`input` can be any value. Text is sent as it is; a struct or a whole row (`b` in `FROM bot_answers b`) is sent as
JSON, so column names reach the model. Send only the columns the question needs: it costs less and reads better.

A `NULL` argument gives a `NULL` answer and makes no call.

`typed_ask` takes one question in typed_ai's own JSON, which every provider reads:

```sql
SELECT typed_ask(answer, '{"v": 1, "type": "pick", "question": "what kind of mistake is this?",
                           "options": ["wrong fact", "wrong rule", "no mistake"],
                           "criteria": {"wrong rule": "misstates what visitors may do"}}') FROM bot_answers;
```

`type` is `yes_no`, `pick` (with `options`) or `score` (with `levels`, lowest first). The result is `JSON`; without
DuckDB's `json` extension loaded, cast it with `::VARCHAR` before comparing it as text.

## Cost controls

| Setting | Default | Scope | Meaning |
| --- | --- | --- | --- |
| `typed_max_rows` | 1000 | database | most rows one query may send; cached rows are free |
| `typed_budget_usd` | 1.00 | database | most dollars spent since the extension loaded |
| `typed_max_input_chars` | 20000 | database | longer inputs are refused, never cut short |
| `typed_dry_run` | false | session | price the query, call nothing, return `NULL` |

- The limits are database-wide: a new connection or Python cursor does not get a fresh budget. An admin can
  freeze them with `SET lock_configuration = true`.
- Each request reserves its estimated cost before it is sent, and the provider's reported tokens replace the
  estimate afterwards. A profile's first request goes alone, and estimates rise at once when a real bill runs over
  them, so the budget is passed by at most the misestimate of the requests in flight.
- A profile's price cannot go below a provider's built-in price, so a `$0` profile cannot switch the Jev budget
  off.
- Spend is never reset from SQL. To go past the budget, raise it.
- Filter cheaply first: in `WHERE created > DATE '2026-09-01' AND typed_is(...)`, DuckDB runs the date check first.
- A call whose arguments are all fixed values, such as `typed_prob('some text', 'q')`, is answered while DuckDB
  plans the query, so `EXPLAIN` on it makes the call. Calls over table columns are not affected.
- `LIMIT` after a `typed_` filter does not bound calls: `WHERE typed_is(...) LIMIT 10` can judge 2,048 rows,
  because DuckDB works in blocks. Limit before judging instead:

```sql
FROM (SELECT * FROM bot_answers LIMIT 10) b WHERE typed_is(b, 'the answer is fully supported by the source');
```

## When a job stops partway

A job stops when it hits the row cap or the budget, when the provider keeps failing, or on Ctrl-C.
`typed_on_stop` picks what you see:

| `typed_on_stop` | The query | An unanswered row |
| --- | --- | --- |
| `'error'` (default) | fails, saying how many rows were answered and what was spent | no rows come back |
| `'null'` | finishes | `NULL` |
| `'row'` | finishes | `{value: NULL, error: 'skipped: budget of $1.00 reached'}` |

In `'row'` mode `typed_prob`, `typed_pick` and `typed_score` return `STRUCT(value, error)`; read
`typed_prob(body, '...').value` and `.error`. `typed_is` stays `BOOLEAN` so it still works in `WHERE`, and
`typed_ask` always carries an `error` field. The mode is fixed when a query is planned.

- Paid answers are never lost. Every answer is saved in memory (`typed_cache_mb`, default 256, about 1.3 million
  answers) the moment it arrives, so running the query again pays only for rows not yet answered. When memory runs
  short, answers from finished queries go first and answers from stopped jobs go last.
- A running query keeps all of its answers, even past `typed_cache_mb`: a 10 million row job holds about 2 GB. Use
  the step pattern below for jobs that large.
- Setup errors always stop the query, in every mode: no key, a rejected key, no price, a bad argument, or an
  answer the extension cannot read.
- Once a job stops, it makes no more calls; every remaining row gets the same reason.
- After `typed_fail_after` failed requests in a row (default 3), calls stop for the rest of the query. The next
  query tries a single request first.
- Ctrl-C cancels the whole query. Answers that arrived before it are kept.
- The cache lives as long as the process. For a job that must survive a crash, write results in steps:

```sql
INSERT INTO results SELECT id, typed_prob(b, 'the answer is fully supported by the source') FROM bot_answers b WHERE id BETWEEN 1 AND 100000;
```

## Profiles

A profile is a DuckDB secret. It holds the provider, model, URL, key and price.

```sql
CREATE SECRET jev   (TYPE typed_ai, PROVIDER 'jev', API_KEY 'ts_...');
CREATE SECRET local (TYPE typed_ai, PROVIDER 'openai', MODEL 'gemma2:2b',
                     URL 'http://localhost:11434/v1/chat/completions', USD_PER_MTOK_IN 0, USD_PER_MTOK_OUT 0);
SET typed_profile = 'local';
```

With no `typed_profile` set, the only `typed_ai` secret is used; with none, the built-in `jev` profile; with
several, the first call lists them. The URL lives only in the secret, next to its key, so no setting can send a
stored key to another server. The URL must use `https`; plain `http` is accepted only for `localhost`, `127.0.0.1`
and `[::1]`, matched exactly, so neither a key nor row data crosses a network unencrypted.

| Provider | Default model | Rows per request | Price (per million tokens) |
| --- | --- | --- | --- |
| `jev` | `jev-latest` | up to 50 (`typed_batch_size`, default 10) | $0.042 in, output free |
| `openai` | none, set `MODEL` | 1 | none, set `USD_PER_MTOK_IN` and `USD_PER_MTOK_OUT` (0 for local) |

The `openai` provider asks for a one-character answer (`Y`/`N`, a letter, or a digit) and reads the model's odds
for it. Models or servers that do not return `logprobs`, such as reasoning models, fail with an error.

## Other settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `typed_concurrency` | 4 | requests in flight across the process; database-wide, like the limits |
| `typed_timeout_s` | 30 | seconds per HTTP attempt |
| `typed_max_retries` | 3 | retries after 408, 429, 5xx or a dropped connection, with backoff and `Retry-After` |
| `typed_fail_after` | 3 | failed requests in a row before calls stop; 0 is off |

HTTP goes through DuckDB's `httpfs`, which is loaded on first use and honours DuckDB's proxy settings.

## Build and test

```sh
GEN=ninja make                         # first build compiles DuckDB too
python3 test/mock_server.py &          # a stand-in for Jev and OpenAI
TYPED_AI_MOCK=http://127.0.0.1:8765 make test
./build/release/duckdb                 # typed_ai is built in; LOAD httpfs happens on first use
```

| Test file | Needs | Cost |
| --- | --- | --- |
| `test/sql/typed_ai.test` | nothing | free |
| `test/sql/typed_ai_mock.test` | `TYPED_AI_MOCK` pointing at `test/mock_server.py` | free |
| `test/sql/typed_ai_live.test` | `TYPED_AI_OLLAMA` and `TYPED_AI_OLLAMA_MODEL`, e.g. `http://localhost:11434` and `gemma2:2b` | free |
| `test/sql/typed_ai_jev.test` | `TYPESAFE_API_KEY` | under $0.001, capped by the test itself |

Other DuckDB builds load the extension file with `duckdb -unsigned` and
`LOAD 'build/release/extension/typed_ai/typed_ai.duckdb_extension'`.
