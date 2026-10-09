# Shader parameter schemas (format `vmt-schema/1`)

One JSON file per VMT shader kind. The files are loaded and validated strictly
by `vmt_core` (`vmt/schema.h`, `SchemaLibrary::load_dir`). Schema "type"
values are semantic metadata for the editor/UI; the loader validates the
schema file itself, not the VMT values of a document.

## Certainty policy

Parameters listed under `groups` are confirmed knowledge. Parameters that
have **not** been verified against the
[Valve Developer Community wiki](https://developer.valvesoftware.com/wiki/Category:Shaders)
must live in `needs_review` instead — never in `groups`. Do not invent
parameter names, ranges or defaults.

To verify one: check the shader's VDC page, then move the entry from
`needs_review` into the appropriate group (with `name`, `type`, optional
`description`/`default`/`values`/`min`/`max`).

## File format

```json
{
  "format": "vmt-schema/1",
  "shader": "VertexLitGeneric",
  "groups": [
    {
      "name": "Base",
      "parameters": [
        {
          "name": "$basetexture",
          "type": "texture",
          "description": "Primary base color texture of the material.",
          "default": "debug/debugempty"
        }
      ]
    }
  ],
  "needs_review": [
    { "name": "$something", "reason": "Verify exact name and type on the VDC wiki." }
  ]
}
```

### Root fields

| Field          | Required | Notes                                                        |
| -------------- | -------- | ------------------------------------------------------------ |
| `format`       | no       | If present, must be exactly `"vmt-schema/1"`.                |
| `shader`       | yes      | Identifier (`[A-Za-z][A-Za-z0-9_]*`); looked up case-insensitively. |
| `groups`       | yes      | Array, may be empty.                                          |
| `needs_review` | no       | Array of `{ "name", "reason"? }`, may be empty.               |

Unknown fields anywhere are validation errors (they usually mean a typo).

### Group

| Field        | Required | Notes                    |
| ------------ | -------- | ------------------------ |
| `name`       | yes      | Non-empty; unique per file (case-insensitive). |
| `parameters` | yes      | Array, may be empty.     |

### Parameter (`groups[].parameters[]`)

| Field         | Required | Notes                                                      |
| ------------- | -------- | ---------------------------------------------------------- |
| `name`        | yes      | `$` or `%` followed by `[A-Za-z0-9_]+`; unique per shader. |
| `type`        | yes      | One of the types below.                                    |
| `default`     | no       | Raw VMT text as a JSON string, e.g. `"1"`, `"[0 0 0]"`.    |
| `description` | no       | String.                                                    |
| `values`      | only for `enum` | Non-empty array of non-empty strings.               |
| `min` / `max` | only for `int`/`float` | JSON numbers; `min <= max` when both present. |
| `advanced`    | no       | Boolean presentation hint, default `false`.                 |

`advanced` is a **presentation hint only**: editors may hide advanced
parameters behind a "Show advanced" toggle. It says nothing about the engine
and must not be used to record uncertainty — unverified parameters belong in
`needs_review` instead. Do not guess it: mark a parameter advanced only when it
is clearly a rarely-tuned/technical knob compared to the rest of its shader.

### Types

`string`, `texture`, `bool`, `int`, `float`, `color`, `vector`, `matrix`,
`enum`.

- `color`: RGB(A) tint; VMT permits `{r g b}`, `{r g b a}` or `[r g b]`.
- `vector`: three components, e.g. `$phongfresnelranges` → `[0 0.5 1]`.
- `matrix`: texture transform, e.g.
  `center .5 .5 scale 1 1 rotate 0 translate 0 0`.

## Validation

Loading fails fast with `SchemaLoadError` (malformed JSON, with line/column)
or `SchemaValidationError` (well-formed JSON violating the rules above).
Duplicate shader names across files are rejected. Loading a directory that
does not exist returns `SchemaNotFound`. Looking up an unknown shader returns
an empty schema — not an error.
