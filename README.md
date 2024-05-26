# templatesvc
Templating service

## Overview

The templatesvc service is a variable templating service which generates
content from templates.  Trigger variables are associated with templates.
When a MODIFIED notification is generated for a trigger variable, the
associated template is rendered and delivered to the specified destination.

## Templatesvc Configuration

The template service takes a configuration file which maps trigger variables
to their associated templates and target output destinations.

## templatesvc template file

An example template file is shown below:

```
------------------------------------------------------------------------------

This is a test of the variable server templating system.

It allows referencing of variables inside a template.

The variables will be replaced with their values rendered by the
variable server.

The value of "/sys/test/a" is "${/sys/test/a}" and the value of "/sys/test/b"
is "${/sys/test/b}".

It can handle variables of all types.

This one is a string: "${/sys/test/c}" and this one is a float: ${/sys/test/f}

------------------------------------------------------------------------------
```

## templatesvc configuration file

The template service is configured with a JSON configuration file
which provides the mapping between the trigger variable name(s), the
name of the template file to be associated with that variable, and the
destination for the rendered output.

An example configuration file is shown below:

```
{
    "config" : [
        { "trigger" : ["/sys/test/info"],
          "template" : "/usr/share/templates/test.tmpl",
          "type" : "fd",
          "destination" : "/splunk",
          "keep_open" : true,
          "append" : true }
    ]
}
```

Note that multiple template mappings can be specified in a single configuration
file, and multiple instances of the template service can be invoked.

## Prerequisites

The template service requires the following components:

- varserver : variable server ( https://github.com/tjmonk/varserver )
- tjson : JSON parser library ( https://github.com/tjmonk/libtjson )

## Building

```
$ ./build.sh
```

