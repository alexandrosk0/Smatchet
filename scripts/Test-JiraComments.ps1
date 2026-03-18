<#
.SYNOPSIS
    Test Jira search vs issue comment API to debug empty comment arrays.
.DESCRIPTION
    Calls the same search endpoint Smatchet uses, then the issue comment endpoint,
    and prints what each returns so you can see where comments are missing.
.EXAMPLE
    .\Test-JiraComments.ps1 -Domain "your-domain.atlassian.net" -Email "you@example.com" -ApiToken "your-token"
#>
param(
    [Parameter(Mandatory = $false)]
    [string] $Domain = $env:JIRA_DOMAIN,
    [Parameter(Mandatory = $false)]
    [string] $Email = $env:JIRA_EMAIL,
    [Parameter(Mandatory = $false)]
    [string] $ApiToken = $env:JIRA_API_TOKEN,
    [Parameter(Mandatory = $false)]
    [string] $Jql = "assignee=currentUser()",
    [Parameter(Mandatory = $false)]
    [int] $MaxIssues = 5
)

$ErrorActionPreference = "Stop"

if (-not $Domain -or -not $Email -or -not $ApiToken) {
    Write-Host "Usage: Provide -Domain, -Email, -ApiToken or set env vars JIRA_DOMAIN, JIRA_EMAIL, JIRA_API_TOKEN"
    Write-Host "Example: .\Test-JiraComments.ps1 -Domain 'mycompany.atlassian.net' -Email 'me@company.com' -ApiToken 'xxx'"
    exit 1
}

$base = $Domain.TrimEnd('/')
if ($base -notmatch '^https?://') { $base = "https://$base" }

$pair = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("${Email}:${ApiToken}"))
$headers = @{
    "Accept"       = "application/json"
    "Authorization" = "Basic $pair"
    "User-Agent"   = "Smatchet-Test-JiraComments/1.0"
}

# Same fields and expand as Smatchet
$fields = "summary,description,status,assignee,priority,issuetype,parent,comment,changelog"
$jqlEncoded = [uri]::EscapeDataString($Jql)
$searchUrl = "${base}/rest/api/3/search/jql?jql=$jqlEncoded&maxResults=100&fields=$fields&expand=changelog"

Write-Host "=== 1. Search (same as Smatchet) ===" -ForegroundColor Cyan
Write-Host "GET $searchUrl"
Write-Host ""

try {
    $searchResp = Invoke-RestMethod -Uri $searchUrl -Headers $headers -Method Get
} catch {
    Write-Host "Search failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.Exception.Response) {
        $reader = New-Object System.IO.StreamReader($_.Exception.Response.GetResponseStream())
        Write-Host $reader.ReadToEnd()
    }
    exit 1
}

$issues = $searchResp.issues
$total = $searchResp.total
Write-Host "Total issues in result: $total"
Write-Host ""

if ($issues.Count -eq 0) {
    Write-Host "No issues returned. Try a different JQL or credentials." -ForegroundColor Yellow
    exit 0
}

$toCheck = [Math]::Min($MaxIssues, $issues.Count)
Write-Host "=== 2. fields.comment from search (first $toCheck issues) ===" -ForegroundColor Cyan
for ($i = 0; $i -lt $toCheck; $i++) {
    $issue = $issues[$i]
    $key = $issue.key
    $fieldsObj = $issue.fields
    $commentField = $fieldsObj.comment
    Write-Host "Issue: $key"
    if (-not $commentField) {
        Write-Host "  fields.comment: (absent)"
    } else {
        $totalComments = $commentField.total
        $commentsArray = $commentField.comments
        $arrLen = if ($commentsArray) { $commentsArray.Count } else { 0 }
        Write-Host "  fields.comment.total:  $totalComments"
        Write-Host "  fields.comment.comments length: $arrLen"
        if ($commentsArray -and $commentsArray.Count -gt 0) {
            Write-Host "  First comment keys: $($commentsArray[0].PSObject.Properties.Name -join ', ')"
        }
    }
    Write-Host ""
}

# Pick first issue and call issue comment endpoint
$firstKey = $issues[0].key
$commentUrl = "${base}/rest/api/3/issue/${firstKey}/comment"
Write-Host "=== 3. Issue comment endpoint (single issue) ===" -ForegroundColor Cyan
Write-Host "GET $commentUrl"
Write-Host ""

try {
    $commentResp = Invoke-RestMethod -Uri $commentUrl -Headers $headers -Method Get
} catch {
    Write-Host "Comment endpoint failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.Exception.Response) {
        try {
            $reader = New-Object System.IO.StreamReader($_.Exception.Response.GetResponseStream())
            Write-Host $reader.ReadToEnd()
        } catch {}
    }
    exit 1
}

$commentTotal = $commentResp.total
$commentList = $commentResp.comments
$commentCount = if ($commentList) { $commentList.Count } else { 0 }
Write-Host "Issue $firstKey - /issue/{key}/comment response:"
Write-Host "  total: $commentTotal"
Write-Host "  comments.Count: $commentCount"
if ($commentList -and $commentList.Count -gt 0) {
    Write-Host "  First comment id: $($commentList[0].id), author: $($commentList[0].author.displayName), body type: $($commentList[0].body.PSObject.Properties.Name -join ', ')"
}

Write-Host ""
Write-Host "Done. If search shows total>0 but comments length 0, Smatchet will use the comment endpoint fallback for that issue." -ForegroundColor Green
