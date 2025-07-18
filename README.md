# PG_NET
*A PostgreSQL extension that enables asynchronous (non-blocking) HTTP/HTTPS requests with SQL*.

Requires libcurl >= 7.83. Compatible with PostgreSQL > = 12.

![PostgreSQL version](https://img.shields.io/badge/postgresql-12+-blue.svg)
[![License](https://img.shields.io/pypi/l/markdown-subtemplate.svg)](https://github.com/supabase/pg_net/blob/master/LICENSE)
[![Coverage Status](https://coveralls.io/repos/github/supabase/pg_net/badge.svg)](https://coveralls.io/github/supabase/pg_net)
[![Tests](https://github.com/supabase/pg_net/actions/workflows/main.yml/badge.svg)](https://github.com/supabase/pg_net/actions)

**Documentation**: [https://supabase.github.io/pg_net](https://supabase.github.io/pg_net)

---

# Contents
- [Introduction](#introduction)
- [Technical Explanation](#technical-explanation)
- [Installation](#installation)
- [Configuration](#extension-configuration)
- [Requests API](#requests-api)
    - Monitoring requests
    - GET requests
    - POST requests
    - DELETE requests
- [Practical Examples](#practical-examples)
    - Syncing data with an external data source using triggers
    - Calling a serverless function every minute with PG_CRON
    - Retrying failed requests
- [Contributing](#contributing)

---

# Introduction

The PG_NET extension enables PostgreSQL to make asynchronous HTTP/HTTPS requests in SQL. It eliminates the need for servers to continuously poll for database changes and instead allows the database to proactively notify external resources about significant events. It seamlessly integrates with triggers, cron jobs (e.g., [PG_CRON](https://github.com/citusdata/pg_cron)), and procedures, unlocking numerous possibilities. Notably, PG_NET powers Supabase's Webhook functionality, highlighting its robustness and reliability.

Common use cases for the PG_NET extension include:

- Calling external APIs
- Syncing data with outside resources
- Calling a serverless function when an event, such as an insert, occurred

However, it is important to note that the extension has a few limitations. Currently, it only supports three types of asynchronous requests:

- async http GET requests
- async http POST requests with a *JSON* payload
- async http DELETE requests

Ultimately, though, PG_NET offers developers more flexibility in how they monitor and connect their database with external resources.

---

# Technical Explanation

The extension introduces a new `net` schema, which contains two unlogged tables, a type of table in PostgreSQL that offers performance improvements at the expense of durability. You can read more about unlogged tables [here](https://pgpedia.info/u/unlogged-table.html). The two tables are:

1. **`http_request_queue`**: This table serves as a queue for requests waiting to be executed. Upon successful execution of a request, the corresponding data is removed from the queue.

    The SQL statement to create this table is:

    ```sql
    CREATE UNLOGGED TABLE
        net.http_request_queue (
            id bigint NOT NULL DEFAULT nextval('net.http_request_queue_id_seq'::regclass),
            method text NOT NULL,
            url text NOT NULL,
            headers jsonb,
            body bytea,
            timeout_milliseconds integer NOT NULL
        )
    ```

2. **`_http_response`**: This table holds the responses of each executed request.

    The SQL statement to create this table is:

    ```sql
    CREATE UNLOGGED TABLE
        net._http_response (
            id bigint NULL,
            status_code integer NULL,
            content_type text NULL,
            headers jsonb NULL,
            content text NULL,
            timed_out boolean NULL,
            error_msg text NULL,
            created timestamp with time zone NOT NULL DEFAULT now()
        )
    ```

When any of the three request functions (`http_get`, `http_post`, `http_delete`) are invoked, they create an entry in the `net.http_request_queue` table.

Once a response is received, it gets stored in the `_http_response` table. By monitoring this table, you can keep track of response statuses and messages.

> [!IMPORTANT]
> Inserting directly into the `net.http_request_queue` won't cause the worker to process requests, you must use the request functions.
> We do it this way to avoid polling the `net.http_request_queue` table, which would pollute `pg_stat_statements` and cause unnecesssary activity from the worker.

The extension employs C's [libcurl](https://curl.se/libcurl/c/) library within a PostgreSQL [background worker](https://www.postgresql.org/docs/current/bgworker.html) to manage HTTP requests.
This background worker sleeps until it receives a signal from the request functions, which awakes it and prompts it to read the `net.http_request_queue` table and execute the requests on it.

---

# Installation

Clone this repo and run

```bash
make && make install
```

To make the extension available to the database add on `postgresql.conf`:

```
shared_preload_libraries = 'pg_net'
```

By default, pg_net is available on the `postgres` database. To use pg_net on a different database, you can add the following on `postgresql.conf`:

```
pg_net.database_name = '<dbname>';
```

Using pg_net on multiple databases in a cluster is not yet supported.

To activate the extension in PostgreSQL, run the create extension command. The extension creates its own schema named `net` to avoid naming conflicts.

```psql
create extension pg_net;
```

---

# Extension Configuration

the extension creates 3 configurable variables:

1. **pg_net.batch_size** _(default: 200)_: An integer that limits the max number of rows that the extension will process from _`net.http_request_queue`_ during each read
2. **pg_net.ttl** _(default: 6 hours)_: An interval that defines the max time a row in the _`net.http_response`_ will live before being deleted. Note that this won't happen exactly after the TTL has passed. The worker will perform this deletion while its processing requests.
3. **pg_net.database_name** _(default: 'postgres')_: A string that defines which database the extension is applied to
4. **pg_net.username** _(default: NULL)_: A string that defines which user will the background worker be connected with. If not set (`NULL`), it will assume the bootstrap user.

All these variables can be viewed with the following commands:
```sql
show pg_net.batch_size;
show pg_net.ttl;
show pg_net.database_name;
show pg_net.username;
```

You can change these by editing the `postgresql.conf` file (find it with `SHOW config_file;`) or with `ALTER SYSTEM`:

```
alter system set pg_net.ttl to '1 hour'
alter system set pg_net.batch_size to 500;
```

Then, you can reload the settings with:

```
select pg_reload_conf();
```

If you change the `pg_net.database_name` and `pg_net.username` configs, you'll need to restart the worker for them to apply.
We provide a function that reloads the config with `pg_reload_conf` and restarts the worker in one go:

```
select net.worker_restart();
```

Note that doing `ALTER SYSTEM` requires SUPERUSER but on PostgreSQL >= 15, you can do:

```
grant alter system on parameter pg_net.ttl to <role>;
grant alter system on parameter pg_net.batch_size to <role>;
```

To allow regular users to update `pg_net` settings.

# Requests API

## GET requests

### net.http_get function signature

```sql
net.http_get(
    -- url for the request
    url text,
    -- key/value pairs to be url encoded and appended to the `url`
    params jsonb default '{}'::jsonb,
    -- key/values to be included in request headers
    headers jsonb default '{}'::jsonb,
    -- the maximum number of milliseconds the request may take before being cancelled
    timeout_milliseconds int default 1000
)
    -- request_id reference
    returns bigint

    strict
    volatile
    parallel safe
    language plpgsql
```

### Examples:
The following examples use the [Postman Echo API](https://learning.postman.com/docs/developer/echo-api/).

#### Calling an API

```sql
SELECT net.http_get (
    'https://postman-echo.com/get?foo1=bar1&foo2=bar2'
) AS request_id;
```

> NOTE: You can view the response with the following query:
>
> ```sql
> SELECT *
> FROM net._http_response;
> ```

#### Calling an API with URL encoded params

```sql
SELECT net.http_get(
  'https://postman-echo.com/get',
  -- Equivalent to calling https://postman-echo.com/get?foo1=bar1&foo2=bar2&encoded=%21
  -- The "!" is url-encoded as %21
  '{"foo1": "bar1", "foo2": "bar2", "encoded": "!"}'::JSONB
) AS request_id;
```

#### Calling an API with an API-KEY

```sql
SELECT net.http_get(
  'https://postman-echo.com/get?foo1=bar1&foo2=bar2',
   headers := '{"API-KEY-HEADER": "<API KEY>"}'::JSONB
) AS request_id;
```

## POST requests
### net.http_post function signature

```sql
net.http_post(
    -- url for the request
    url text,
    -- body of the POST request
    body jsonb default '{}'::jsonb,
    -- key/value pairs to be url encoded and appended to the `url`
    params jsonb default '{}'::jsonb,
    -- key/values to be included in request headers
    headers jsonb default '{"Content-Type": "application/json"}'::jsonb,
    -- the maximum number of milliseconds the request may take before being cancelled
    timeout_milliseconds int default 1000
)
    -- request_id reference
    returns bigint

    volatile
    parallel safe
    language plpgsql
```
### Examples:
The following examples post to the [Postman Echo API](https://learning.postman.com/docs/developer/echo-api/).

#### Sending data to an API

```sql
SELECT net.http_post(
    'https://postman-echo.com/post',
    '{"key": "value", "key": 5}'::JSONB,
    headers := '{"API-KEY-HEADER": "<API KEY>"}'::JSONB
) AS request_id;
```

#### Sending single table row as a payload

> NOTE: If multiple rows are sent using this method, each row will be sent as a separate request.

```sql
WITH selected_row AS (
    SELECT
        *
    FROM target_table
    LIMIT 1
)
SELECT
    net.http_post(
        'https://postman-echo.com/post',
        to_jsonb(selected_row.*),
        headers := '{"API-KEY-HEADER": "<API KEY>"}'::JSONB
    ) AS request_id
FROM selected_row;
```

#### Sending multiple table rows as a payload

> WARNING: when sending multiple rows, be careful to limit your payload size.

```sql
WITH selected_rows AS (
    SELECT
        -- Converts all the rows into a JSONB array
        jsonb_agg(to_jsonb(target_table)) AS JSON_payload
    FROM target_table
    -- Generally good practice to LIMIT the max amount of rows
)
SELECT
    net.http_post(
        'https://postman-echo.com/post'::TEXT,
        JSON_payload,
        headers := '{"API-KEY-HEADER": "<API KEY>"}'::JSONB
    ) AS request_id
FROM selected_rows;
```

## DELETE requests
### net.http_delete function signature

```sql
net.http_delete(
    -- url for the request
    url text,
    -- key/value pairs to be url encoded and appended to the `url`
    params jsonb default '{}'::jsonb,
    -- key/values to be included in request headers
    headers jsonb default '{}'::jsonb,
    -- the maximum number of milliseconds the request may take before being cancelled
    timeout_milliseconds int default 2000
)
    -- request_id reference
    returns bigint

    strict
    volatile
    parallel safe
    language plpgsql
    security definer
```

### Examples:
The following examples use the [Dummy Rest API](https://dummy.restapiexample.com/employees).

#### Sending a delete request to an API

```sql
SELECT net.http_delete(
    'https://dummy.restapiexample.com/api/v1/delete/2'
) AS request_id;
```

#### Sending a delete request with a row id as a query param

```sql
WITH selected_id AS (
    SELECT
        id
    FROM target_table
    LIMIT 1 -- if not limited, it will make a delete request for each returned row
)
SELECT
    net.http_delete(
        'https://dummy.restapiexample.com/api/v1/delete/'::TEXT,
        format('{"id": "%s"}', id)::JSONB
    ) AS request_id
FROM selected_id;
```

#### Sending a delete request with a row id as a path param

```sql
WITH selected_id AS (
    SELECT
        id
    FROM target_table
    LIMIT 1 -- if not limited, it will make a delete request for each returned row
)
SELECT
    net.http_delete(
        'https://dummy.restapiexample.com/api/v1/delete/' || id
    ) AS request_id
FROM selected_row
```

---

# Practical Examples

## Syncing data with an external data source using triggers

The following example comes from [Typesense's Supabase Sync guide](https://typesense.org/docs/guide/supabase-full-text-search.html#syncing-individual-deletes)

```sql
-- Create the function to delete the record from Typesense
CREATE OR REPLACE FUNCTION delete_record()
    RETURNS TRIGGER
    LANGUAGE plpgSQL
AS $$
BEGIN
    SELECT net.http_delete(
        url := format('<TYPESENSE URL>/collections/products/documents/%s', OLD.id),
        headers := '{"X-Typesense-API-KEY": "<Typesense_API_KEY>"}'
    )
    RETURN OLD;
END $$;

-- Create the trigger that calls the function when a record is deleted from the products table
CREATE TRIGGER delete_products_trigger
    AFTER DELETE ON public.products
    FOR EACH ROW
    EXECUTE FUNCTION delete_products();
```

## Calling a serverless function every minute with PG_CRON

The [PG_CRON](https://github.com/citusdata/pg_cron) extension enables PostgreSQL to become its own cron server. With it you can schedule regular calls to activate serverless functions.

> Useful links:
>
> * [Cron Syntax Helper](https://crontab.guru/)

### Example Cron job to call serverless function

```sql
SELECT cron.schedule(
	'cron-job-name',
	'* * * * *', -- Executes every minute (cron syntax)
	$$
	    -- SQL query
	    SELECT net.http_get(
		-- URL of Edge function
		url:='https://<reference id>.functions.supabase.co/example',
		headers:='{
		    "Content-Type": "application/json",
		    "Authorization": "Bearer <TOKEN>"
		}'::JSONB
	    ) as request_id;
	$$
);
```

## Retrying failed requests

Every request made is logged within the net._http_response table. To identify failed requests, you can execute a query on the table, filtering for requests where the status code is 500 or higher.

### Finding failed requests

```sql
SELECT
    *
FROM net._http_response
WHERE status_code >= 500;
```

While the net.\_http_response table logs each request, it doesn't store all the necessary information to retry failed requests. To facilitate this, we need to create a request tracking table and a wrapper function around the PG_NET request functions. This will help us store the required details for each request.

### Creating a Request Tracker Table

```sql
CREATE TABLE request_tracker(
    method TEXT,
    url TEXT,
    params JSONB,
    body JSONB,
    headers JSONB,
    request_id BIGINT
)
```

Below is a function called request_wrapper, which wraps around the PG_NET request functions. This function records every request's details in the request_tracker table, facilitating future retries if needed.

### Creating a Request Wrapper Function

```sql
CREATE OR REPLACE FUNCTION request_wrapper(
    method TEXT,
    url TEXT,
    params JSONB DEFAULT '{}'::JSONB,
    body JSONB DEFAULT '{}'::JSONB,
    headers JSONB DEFAULT '{}'::JSONB
)
RETURNS BIGINT
AS $$
DECLARE
    request_id BIGINT;
BEGIN

    IF method = 'DELETE' THEN
        SELECT net.http_delete(
            url:=url,
            params:=params,
            headers:=headers
        ) INTO request_id;
    ELSIF method = 'POST' THEN
        SELECT net.http_post(
            url:=url,
            body:=body,
            params:=params,
            headers:=headers
        ) INTO request_id;
    ELSIF method = 'GET' THEN
        SELECT net.http_get(
            url:=url,
            params:=params,
            headers:=headers
        ) INTO request_id;
    ELSE
        RAISE EXCEPTION 'Method must be DELETE, POST, or GET';
    END IF;

    INSERT INTO request_tracker (method, url, params, body, headers, request_id)
    VALUES (method, url, params, body, headers, request_id);

    RETURN request_id;
END;
$$
LANGUAGE plpgsql;
```

To retry a failed request recorded via the wrapper function, use the following query. This will select failed requests, retry them, and then remove the original request data from both the net.\_http_response and request_tracker tables.

### Retrying failed requests

```sql
WITH retry_request AS (
    SELECT
        request_tracker.method,
        request_tracker.url,
        request_tracker.params,
        request_tracker.body,
        request_tracker.headers,
        request_tracker.request_id
    FROM request_tracker
    INNER JOIN net._http_response ON net._http_response.id = request_tracker.request_id
    WHERE net._http_response.status_code >= 500
    LIMIT 3
),
retry AS (
    SELECT
        request_wrapper(retry_request.method, retry_request.url, retry_request.params, retry_request.body, retry_request.headers)
    FROM retry_request
),
delete_http_response AS (
    DELETE FROM net._http_response
    WHERE id IN (SELECT request_id FROM retry_request)
    RETURNING *
)
DELETE FROM request_tracker
WHERE request_id IN (SELECT request_id FROM retry_request)
RETURNING *;
```

The above function can be called using cron jobs or manually to retry failed requests. It may also be beneficial to clean the request_tracker table in the process.

# Contributing

Checkout the [Contributing](docs/contributing.md) page to learn more about adding to the project.
