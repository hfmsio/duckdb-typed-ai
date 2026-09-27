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

SELECT * FROM tickets t WHERE typed_is(t.body, 'the customer wants a refund');
SELECT body, typed_pick(body, 'which team should handle this?', ['billing', 'technical', 'sales']) FROM tickets;
FROM typed_usage();
```

With no secret, the built-in `jev` profile reads `TYPESAFE_API_KEY` from the environment.

## Walkthrough for a first session

A first session from start to finish on DuckDB v1.5.5. The outputs come from real runs, except step 7, which shows
example numbers. Probabilities can differ a little between runs and model versions.

### 1. Start DuckDB and load the extension

A local build is not signed by DuckDB, so start the CLI with `-unsigned`. The API key comes from
`TYPESAFE_API_KEY` in your environment (for example `export TYPESAFE_API_KEY=...` in `~/.zshrc`), so nothing else
is needed.

```text
$ duckdb -unsigned
D LOAD '/path/to/duckdb-jev-ext/build/release/extension/typed_ai/typed_ai.duckdb_extension';
```

### 2. Make a small table

```sql
CREATE TABLE tickets AS SELECT * FROM (VALUES
  ('card declined again, I want my money back'),
  ('how do I export a dashboard to PDF?')) t(body);
```

### 3. Keep the rows where the answer is yes

`typed_is` returns true or false, so it works in `WHERE`.

```sql
SELECT * FROM tickets WHERE typed_is(body, 'the customer wants a refund');
```

```text
┌───────────────────────────────────────────┐
│                   body                    │
│                  varchar                  │
├───────────────────────────────────────────┤
│ card declined again, I want my money back │
└───────────────────────────────────────────┘
```

### 4. See how sure the model is

`typed_prob` returns the probability of yes, from 0 to 1. Look at these before picking a threshold for `typed_is`
(the default is 0.5).

```sql
SELECT body, typed_prob(body, 'the customer wants a refund') AS refund_chance FROM tickets;
```

```text
┌───────────────────────────────────────────┬───────────────┐
│                   body                    │ refund_chance │
│                  varchar                  │    double     │
├───────────────────────────────────────────┼───────────────┤
│ card declined again, I want my money back │          0.96 │
│ how do I export a dashboard to PDF?       │          0.03 │
└───────────────────────────────────────────┴───────────────┘
```

### 5. Pick one option

`typed_pick` returns one of the options, exactly as written.

```sql
SELECT body, typed_pick(body, 'which team should handle this?', ['billing', 'technical', 'sales']) AS team
FROM tickets;
```

```text
┌───────────────────────────────────────────┬───────────┐
│                   body                    │   team    │
│                  varchar                  │  varchar  │
├───────────────────────────────────────────┼───────────┤
│ card declined again, I want my money back │ billing   │
│ how do I export a dashboard to PDF?       │ technical │
└───────────────────────────────────────────┴───────────┘
```

### 6. Score on a scale

`typed_score` returns a position on the levels you list, lowest first: here 0 is calm and 2 is furious. The score
can land between levels.

```sql
SELECT body, typed_score(body, 'how angry is the customer?', ['calm', 'annoyed', 'furious']) AS anger
FROM tickets;
```

```text
┌───────────────────────────────────────────┬────────┐
│                   body                    │ anger  │
│                  varchar                  │ double │
├───────────────────────────────────────────┼────────┤
│ card declined again, I want my money back │    1.3 │
│ how do I export a dashboard to PDF?       │    0.0 │
└───────────────────────────────────────────┴────────┘
```

### 7. Check what you have spent

`typed_usage()` has 19 columns; pick the ones you want to read.

```sql
SELECT profile, model, requests, printf('$%.6f', spent_usd) AS spent, budget_usd FROM typed_usage();
```

```text
┌────────────────┬────────────┬──────────┬───────────┬────────────┐
│    profile     │   model    │ requests │   spent   │ budget_usd │
│    varchar     │  varchar   │  int64   │  varchar  │   double   │
├────────────────┼────────────┼──────────┼───────────┼────────────┤
│ jev (built in) │ jev-latest │        3 │ $0.000060 │        1.0 │
└────────────────┴────────────┴──────────┴───────────┴────────────┘
```

Example numbers: steps 3 to 6 take about 3 requests, because rows are sent 10 to a request and `typed_is` and
`typed_prob` share one saved answer when they ask the same question.

Asking the same question about the same row again is free: answers are saved in memory.

### 8. Price a big table before running it

With more than 1,000 rows the query is refused before anything is sent:

```sql
CREATE TABLE big_table AS SELECT 'support ticket number ' || i AS body FROM range(5000) r(i);
SELECT typed_prob(body, 'the customer wants a refund') FROM big_table;
```

```text
Invalid Input Error: typed_ai: this query would send more than 1000 rows to the model (typed_max_rows).
To allow more: SET typed_max_rows = N. ...
```

A dry run prices it without calling anything:

```sql
SET typed_dry_run = true;
SET typed_max_rows = 5000;
SELECT count(typed_prob(body, 'the customer wants a refund')) FROM big_table;
SELECT printf('$%.4f', dry_run_usd) AS would_cost, requests AS calls_made FROM typed_usage();
SET typed_dry_run = false;
```

```text
┌────────────┬────────────┐
│ would_cost │ calls_made │
│  varchar   │   int64    │
├────────────┼────────────┤
│ $0.0092    │          0 │
└────────────┴────────────┘
```

If the price is fine, run the same query with `typed_dry_run` off. The budget (`typed_budget_usd`, default $1)
still stops it if the estimate was low.

### 9. Use a free local model instead

With [Ollama](https://ollama.com) running and a model pulled (`ollama pull gemma2:2b`), a profile points the same
functions at it. Nothing is billed.

```sql
CREATE SECRET local (TYPE typed_ai, PROVIDER 'openai', MODEL 'gemma2:2b',
  URL 'http://localhost:11434/v1/chat/completions', USD_PER_MTOK_IN 0, USD_PER_MTOK_OUT 0);
SET typed_profile = 'local';
SELECT body, typed_pick(body, 'which team should handle this?', ['billing', 'technical', 'sales']) AS team
FROM tickets;
```

```text
┌───────────────────────────────────────────┬───────────┐
│                   body                    │   team    │
│                  varchar                  │  varchar  │
├───────────────────────────────────────────┼───────────┤
│ card declined again, I want my money back │ billing   │
│ how do I export a dashboard to PDF?       │ technical │
└───────────────────────────────────────────┴───────────┘
```

To go back to Jev: `RESET typed_profile;` and `DROP SECRET local;`. With two secrets and no `typed_profile`, the
first call asks you to pick one.

### 10. See the full answer

`typed_ask` returns everything the model said, as JSON: the choice, the probability of each option, the model
version, and whether its odds are calibrated (Jev's are; a local model's are not).

```sql
SELECT typed_ask(body, '{"v": 1, "type": "pick", "question": "which team?", "options": ["billing", "technical"]}')
FROM tickets LIMIT 1;
```

```text
{"type":"pick","choice":"billing","probabilities":{"billing":0.9928,"technical":0.0072},"model":"gemma2:2b","calibrated":false,"error":null}
```

(Probabilities shortened here; the real output has full precision.)

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
| `typed_pick(input, question, options)` | `VARCHAR`: the most likely option, exactly as given |
| `typed_score(input, question, levels)` | `DOUBLE`: probability-weighted level, 0 to n-1 |
| `typed_ask(input, question_json)` | `JSON`: the full answer with probabilities, confidence, model and whether it is calibrated |
| `typed_usage()` | one row: spend, budget, requests, retries, cache, breaker, and the last query's counts |

`input` can be any value. Text is sent as it is; a struct or a whole row (`t` in `FROM tickets t`) is sent as
JSON, so column names reach the model. Send only the columns the question needs: it costs less and reads better.

A `NULL` argument gives a `NULL` answer and makes no call.

`typed_ask` takes one question in typed_ai's own JSON, which every provider reads:

```sql
SELECT typed_ask(body, '{"v": 1, "type": "pick", "question": "Which team?",
                         "options": ["billing", "technical"],
                         "criteria": {"billing": "payments, refunds, invoices"}}') FROM tickets;
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
FROM (SELECT * FROM tickets LIMIT 10) t WHERE typed_is(t.body, 'wants a refund');
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
INSERT INTO results SELECT id, typed_prob(body, 'wants a refund') FROM tickets WHERE id BETWEEN 1 AND 100000;
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
