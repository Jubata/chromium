# Network Traffic Annotations

[TOC]

This document presents a practical guide to using network traffic annotations in
Chrome.


## Problem Statement

To make Chrome’s network communication transparent, we would need to be able to
provide the following answers:
* What is the intent behind each network request?
* What user data is sent in the request, and where does it go?

Besides these requirements, the following information helps Enterprise admins
and help desk:
* How can a network communication be stopped or controlled?
* What are the traces of the communication on the client?

It should be noted that the technical details of requests are not necessarily
important to the users, but in order to provide the intended transparency, we
need to show that we have covered all bases and there are no back doors.


## The Solution

We can provide up to date, in-line documentation on origin, intent, payload, and
control mechanisms of each network communication. This is done by adding a
`NetworkTrafficAnnotationTag` to all network communication functions.
Please note that as the goal is to specify the intent behind each network
request and its payload, this metadata does not need to be transmitted with the
request during runtime and it is sufficient to have it in appropriate positions
in the code. Having that as an argument of all network communication functions
is a mechanism to enforce its existence and showing the users our intent to
cover the whole repository.


## Best Practices

### Where to add annotation?
All network requests are ultimately sending data through sockets or native API
functions, but we should note that the concern is about the main intent of the
communication and not the implementation details. Therefore we do not need to
specify this data separately for each call to each function that is used in the
process and it is sufficient that the most rational point of origin would be
annotated and the annotation would be passed through the downstream steps.
Best practices for choosing annotation code site include where:
  1. The origin of user’s intent or internal requirement for the request is
      stated.
  2. The controls over stopping or limiting the request (Chrome settings or
     policies) are enforced.
  3. The data that is sent is specified.
If there is a conflict between where is the best annotation point, please refer
to the `Partial Annotations` section for an approach to split annotation.

### Merged Requests
There are cases where requests are received from multiple sources and merged
into one connection, like when a socket merges several data frames and sends
them together, or a device location is requested by different components, and
just one network request is made to fetch it. In these cases, the merge point
can ensure that all received requests are properly annotated and just pass one
of them to the downstream step.
This decision is driven from the fact that we do not need to transmit the
annotation metadata in runtime and enforced annotation arguments are just to
ensure that the request is annotated somewhere upstream.


## Coverage
Network traffic annotations are currently enforced on all url requests in
Windows and Linux, and are expanding to sockets and native API functions in
2017,Q4 - 2018,Q1.
Currently there is no plan to expand the task to other platforms.


## Network Traffic Annotation Tag

`net::NetworkTrafficAnnotationTag` is the main definition for annotations. There
are few variants of it that are specified in later sections. The goal is to have
one object of this type or its variants as an argument of all functions that
create a network request.

### Content of Annotation Tag
Each network traffic annotation should specify the following items:
* `uniqueـid`: A globally unique identifier that must stay unchanged while the
  network request carries the same semantic meaning. If the network request gets
  a new meaning, this ID needs to be changed. The purpose of this ID is to give
  humans a chance to reference NetworkTrafficAnnotations externally even when
  those change a little bit (e.g. adding a new piece of data that is sent along
  with a network request). IDs of one component should have a shared prefix so
  that sorting all NetworkTrafficAnnotations by unique_id groups those that
  belong to the same component together.
* `TrafficSource`: These set of fields specify the location of annotation in
  the source code. These fields are automatically set and do not need
  specification.
* `TrafficSemantics`: These set of fields specify meta information about the
  network request’s content and reason.
   * `sender`: What component triggers the request. The components should be
     human readable and don’t need to reflect the components/ directory. Avoid
     abbreviations.
   * `description`: Plaintext description of the network request in language
     that is understandable by admins (ideally also users). Please avoid
     acronyms and describe the feature and the    feature's value proposition as
     well.
   * `trigger`: What user action triggered the network request. Use a textual
     description. This should be a human readable string.
   * `data`: What nature of data is being sent. This should be a human readable
     string. Any user data and/or PII should be pointed out.
   * `destination`: Target of the network request. It can be either the website
     that user visits and interacts with, a Google service, a request that does
     not go to network and just fetches a local resource, or other endpoints
     like a service hosting PAC scripts. The distinction between a Google owned
     service and website can be difficult when the user navigates to google.com
     or searches with the omnibar. Therefore follow the following guideline: If
     the source code has hardcoded that the request goes to Google (e.g. for
     ZeroSuggest), use  `GOOGLE_OWNED_SERVICE`. If the request can go to other
     domains and is perceived as a part of a website rather than a native
     browser feature, use `WEBSITE`. Use `LOCAL` if the reques is processed
     locally and doesn't go to network, otherwise use `OTHER`. If `OTHER` is
     used, please add plain text description in `destination_other`
     field.
   * `destination_other`: Human readable description in case the destination
     points to `OTHER`.
* `TrafficPolicy`: These set of fields specify the controls that a user may have
  on disabling or limiting the network request and its trace.
   * `empty_policy_justification`: If traffic policy cannot be specified, like
     when the request originates from inside network stack and none of the below
     control policies applies to it, a plain text here can specify it.
   * `cookies_allowed`: Specifies if this request stores and uses cookies or
     not. Use values `YES` or `NO`.
   * `cookies_store`: If a request sends or stores cookies/channel IDs/... (i.e.
     if `cookies_allowed` is true), we want to know which cookie store is being
     used. The answer to this question can typically be derived from the
     URLRequestContext that is being used. The three most common cases will be:
      * If `cookies_allowed` is false, leave this field unset.
      * If the profile's default URLRequestContext is being used (e.g. from
        `Profile::GetRequestContext())`, this means that the user's normal
        cookies sent. In this case, put `user` here.
      * If the system URLRequestContext is being used (for example via
        `io_thread()->system_url_request_context_getter())`, put `system` here.
      * Otherwise, please explain (e.g. SafeBrowsing uses a separate cookie
        store).
   * `setting`: Human readable description of how to enable/disable a feature
     that triggers this network request by a user (e.g. “Disable ‘Use a web
     service to help resolve spelling errors.’ in settings under Advanced”).
     Note that settings look different on different platforms, make sure your
     description works everywhere!
   * `chrome_policy`: Policy configuration that disables or limits this network
     request. This would be a text serialized protobuf of any enterprise policy.
     See policy list or  chrome_settings.proto for the full list of policies.
   * `policy_exception_justification`: If there is no policy to disable or limit
     this request, a justification can be presented here. In case ‘Empty Policy
     Justification’ is presented, this field is not required.
* `comments`: If required, any human readable extra comments.

### Format and Examples
Traffic annotations are kept in code as serialized protobuf. To define a
`NetworkTrafficAnnotationTag`, you may use the function
`net::DefineNetworkTrafficAnnotation`, with two arguments, the unique id, and
all other fields bundled together as a serialized protobuf string.

#### Good examples
```cpp
  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("spellcheck_lookup", R"(
        semantics {
          sender: "Online Spellcheck"
          description:
            "Chrome can provide smarter spell-checking by sending text you "
            "type into the browser to Google's servers, allowing you to use "
            "the same spell-checking technology used by Google products, such "
            "as Docs. If the feature is enabled, Chrome will send the entire "
            "contents of text fields as you type in them to Google along with "
            "the browser’s default language. Google returns a list of "
            "suggested spellings, which will be displayed in the context menu."
          trigger: "User types text into a text field or asks to correct a "
                   "misspelled word."
          data: "Text a user has typed into a text field. No user identifier "
                "is sent along with the text."
          destination: GOOGLE_OWNED_SERVICE
        }
        policy {
          cookies_allowed: NO
          setting:
            "You can enable or disable this feature via 'Use a web service to "
            "help resolve spelling errors.' in Chrome's settings under "
            "Advanced. The feature is disabled by default."
          chrome_policy {
            SpellCheckServiceEnabled {
                SpellCheckServiceEnabled: false
            }
          }
        })");
```

```cpp
  net::NetworkTrafficAnnotationTag traffic_annotation2 =
      net::DefineNetworkTrafficAnnotation(
        "safe_browsing_chunk_backup_request",
        R"(
        semantics {
          sender: "Safe Browsing"
          description:
            "Safe Browsing updates its local database of bad sites every 30 "
            "minutes or so. It aims to keep all users up-to-date with the same "
            "set of hash-prefixes of bad URLs."
          trigger:
            "On a timer, approximately every 30 minutes."
          data:
            "The state of the local DB is sent so the server can send just the "
            "changes. This doesn't include any user data."
          destination: GOOGLE_OWNED_SERVICE
        }
        policy {
          cookies_allowed: YES
          cookies_store: "Safe Browsing cookie store"
          setting:
            "Users can disable Safe Browsing by unchecking 'Protect you and "
            "your device from dangerous sites' in Chromium settings under "
            "Privacy. The feature is enabled by default."
          chrome_policy {
            SafeBrowsingEnabled {
              policy_options {mode: MANDATORY}
              SafeBrowsingEnabled: false
            }
          }
        })");
```

#### Bad Examples
```cpp
  net::NetworkTrafficAnnotationTag bad_traffic_annotation =
      net::DefineNetworkTrafficAnnotation(
        ...
        trigger: "Chrome sends this when [obscure event that is not related to "
                 "anything user-perceivable]."
          // Please specify the exact user action that results in this request.
        data: "This sends everything the feature needs to know."
          // Please be precise, name the data items. If they are too many, name
          // the sensitive user data and general classes of other data and refer
          // to a document specifying the details.
        ...
        policy_exception_justification: "None."
          // Check again! Most features can be disabled or limited by a policy.
        ...
        })");
```

#### Empty Template
You can copy/paste the following template to define an annotation.
```cpp
  net::NetworkTrafficAnnotationTag traffic_annotation =
    net::DefineNetworkTrafficAnnotation("...", R"(
        semantics {
          sender: "..."
          description: "..."
          trigger: "..."
          data: "..."
          destination: WEBSITE/GOOGLE_OWNED_SERVICE/OTHER
        }
        policy {
          empty_policy_justification = "..."
          cookies_allowed: NO/YES
          cookies_store: "..."
          setting: "..."
          chrome_policy {
            [POLICY_NAME] {
                [POLICY_NAME]: ...
            }
          }
          policy_exception_justification = "..."
        }
        comments: "..."
      )");
```


## Testing for errors

There are several checks that should be done on annotations before submitting a
change list. These checks include:
* The annotations are syntactically correct.
* They have all required fields.
* Partial annotations and completing parts match (please refer to the next
  section).
* Annotations are not incorrectly defined.
   * e.g., traffic_annotation = NetworkTrafficAnnotation({1}).
* All usages from Chrome have annotation.
* Unique ids are unique, through history (even if an annotation gets deprecated,
  its unique id cannot be reused to keep the stats sound).

To do these tests, traffic_annotation_auditor binary runs over the whole
repository and using a clang tool, checks if all above items are correct.
Running the `traffic_annotation_auditor` requires exiting a compiled build
directory and can be done with the following syntax.
`tools/traffic_annotation/bin/[linux64/windows32/mac]/traffic_annotation_auditor
 --build-path=[out/Default]`
If you are running the auditor on Windows, please refer to extra instructions in
`tools/traffic_annotation/auditor/README.md`.
As this test is slow, it is not a mandatory step of the presubmit checks on
clients, and one can run it manually. The test is done on trybots as a commit
queue step.


## Annotations Review

Network traffic annotations require review by privacy, enterprise, and legal
teams. To shorten the process of review, only privacy review is a blocking step
and review by the other two teams will be done after code submission.
Privacy reviews are enforced through keeping a summary of annotations in
`tools/traffic_annotation/summary/annotations.xml`, which is owned by privacy
team. Once a new annotation is added, one is updated, or deleted, this file
should also be updated. To update the file automatically, one can run
`traffic_annotation_auditor` as specified in above step. But if it is not
possible to do so (e.g., if you are changing the code from an unsupported
platform or you don’t have a compiled build directory), the code can be
submitted to the trybot and the test on trybot will tell you the required
modifications.


## Partial Annotations (Advanced)

There are cases where the network traffic annotation cannot be fully specified
in one place. For example, in one place we know the trigger of a network request
and in another place we know the data that will be sent. In these cases, we
prefer that both parts of the annotation appear in context so that they are
updated if code changes. Partial annotations help splitting the network traffic
annotation into two pieces. In these cases, we call the first part, the partial
annotation, and the part the completes it, the completing annotation. Partial
annotations and completing annotations do not need to have all annotation
fields, but their composition should have all required fields.

### Defining a Partial Annotation
To define a partial annotation, one can use
`net::DefinePartialNetworkTrafficAnnotation` function. Besides the unique id and
annotation text, this function requires the unique id of the completing part.
For example, a partial annotation that only specifies the semantics part or a
request with unique id "omnibox_prefetch_image", and is completed later using an
annotation with unique id "bitmap_fetcher", can be defined as follows:

```cpp
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation("omnibox_prefetch_image",
                                                 "bitmap_fetcher", R"(
        semantics {
          sender: "Omnibox"
          Description: "..."
          Trigger: "..."
          Data: "..."
          destination: WEBSITE
        })");
```

### Nx1 Partial Annotations
The cases where several partial annotations may be completed by one completing
annotation are called Nx1. This also matches where N=1. To define a completing
annotation for such cases, one can use net::CompleteNetworkTrafficAnnotation
function. This function receives a unique id, the annotation text, and a
`net::PartialNetworkTrafficAnnotationTag` object. Here is an example of a
completing part for the previous example:

```cpp
  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::CompleteNetworkTrafficAnnotation("bitmap_fetcher",
                                            partial_traffic_annotation, R"(
              policy {
                cookies_allowed: YES
                cookies_store: "user"
                setting: "..."
                chrome_policy {...}
              })");
```

### 1xN Partial Annotations
There are cases where one partial traffic annotation may be completed by
different completing annotations. In these cases,
`net::BranchedCompleteNetworkTrafficAnnotation` function can be used. This
function has an extra argument that is common between all branches and is
referred to by the partial annotation. For the above examples, if there would be
two different ways of completing the received partial annotation, the following
the definition can be used:

```cpp
if (...) {
    return net::BranchedCompleteNetworkTrafficAnnotation(
        "bitmap_fetcher_type1", "bitmap_fetcher",
        partial_traffic_annotation, R"(
          policy {
            cookies_allowed: YES
            cookies_store: "user"
            setting: "..."
            chrome_policy {...}
          })");
  } else {
    return net::BranchedCompleteNetworkTrafficAnnotation(
        "bitmap_fetcher_type2", "bitmap_fetcher",
        partial_traffic_annotation, R"(
          policy {
            cookies_allowed: YES
            cookies_store: "system"
            setting: "..."
            chrome_policy {...}
          })");
```

Please refer to `tools/traffic_annotation/sample_traffic_annotation.cc` for more
detailed examples.


## Mutable Annotations (Advanced)

`net::NetworkTrafficAnnotationTag` and `net::PartialNetworkTrafficAnnotationTag`
are defined with constant internal argument(s), so that once they are created,
they cannot be modified. There are very few exceptions that may require
modification of the annotation value, like the ones used by mojo interfaces
where after serialization, the annotation object is first created, then receives
value. In these cases, `net::MutableNetworkTrafficAnnotationTag` and
`net::MutablePartialNetworkTrafficAnnotationTag` can be used which do not have
this limitation. It is strongly suggested that mutable annotations would be used
only if there is no other way around it. Use cases are checked with the
`traffic_annotation_auditor` to ensure proper values for the mutable
annotations.