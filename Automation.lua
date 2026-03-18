-- Smatchet Automation Script
log_info("Running Scan for: " .. ticket.id)

-- Logic: If the title contains 'Crash' or 'Critical', escalate it.
local title = ticket.title:lower()

if title:find("crash") or title:find("fatal") then
    ticket.status = "🔥 CRITICAL"
    log_info("Escalated ticket " .. ticket.id .. " due to crash keywords.")
end

if title:find("ui") then
    log_info("Flagging " .. ticket.id .. " for UI team review.")
end