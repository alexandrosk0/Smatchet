-- Runs once per active ticket when you click "Run Lua Automation".
-- The host calls `process_ticket(ticket)`.

function process_ticket(ticket)
    log_info("Automation.lua stub for ticket " .. tostring(ticket.id))
end
