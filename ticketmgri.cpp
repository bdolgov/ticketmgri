#include <api/action.h>
#include <api/module.h>
#include <api/stdconfig.h>
#include <billmgr/db.h>
#include <mgr/mgrdb_struct.h>
#include <mgr/mgrlog.h>
#include <mgr/mgrtask.h>

MODULE("ticketmgri");

using namespace isp_api;

namespace {

StringVector allowedDepartments, hideDepartments;

void SyncTicket(int _id) {
  string id = str::Str(_id);
  Warning("Sync %s", id.c_str());
  if (!_id) return;
  mgr_task::LongTask("sbin/ticketmgri_syncticket", "ticket_" + id,
                     "ticketmgri_sync")
      .SetParam(id)
      .Start();
}

struct eTicketEdit : public Event {
  eTicketEdit(const string &ev, const string &elid_name = "elid")
      : Event(ev, "ticketmgri_" + ev), elid_name_(elid_name) {
    Warning("eTicketEdit created");
  }

  void AfterExecute(Session &ses) const override {
    Warning("subm %d cb %s elid %s", ses.IsSubmitted(),
            ses.Param("clicked_button").c_str(), ses.Param("elid").c_str());
    string button = ses.Param("clicked_button");

    string elid;
    if (elid_name_ == "elid_ticket2user") {
      elid = db->Query("SELECT ticket FROM ticket2user WHERE id='" +
                       ses.Param("elid") + "'")
                 ->Str();
    } else {
      elid = ses.Param("elid");
    }

    if ((ses.IsSubmitted() || ses.Param("sv_field") == "ok_message") &&
        (button == "ok" || button == "" || button == "ok_message")) {
      if (!ses.Has(elid_name_)) {
        SyncTicket(db->Query("SELECT MAX(id) FROM ticket")->Int());
      } else {
        SyncTicket(str::Int(elid));
      }
    }
  }

  string elid_name_;
};

struct eClientTicketEdit : public eTicketEdit {
  eClientTicketEdit() : eTicketEdit("clientticket.edit") {}

  void AfterExecute(Session &ses) const override {
    eTicketEdit::AfterExecute(ses);
    for (auto &i : hideDepartments) {
      ses.xml.RemoveNodes("//slist[@name='client_department']/val[@key='" + i +
                          "']");
    }
  }
};

struct aTicketintegrationSetFilter : public Action {
  aTicketintegrationSetFilter()
      : Action("ticketintegration.setfilter", MinLevel(lvAdmin)) {}

  void Execute(Session &ses) const override {
    InternalCall(ses, "account.setfilter", "elid=" + ses.Param("elid"));
    ses.Ok(ses.okTop);
  }
};

struct aTicketintegrationPost : public Action {
  aTicketintegrationPost()
      : Action("ticketintegration.post", MinLevel(lvAdmin)) {}

  void Execute(Session &ses) const override { Execute(ses, true); }

  void Execute(Session &ses, bool retry) const {
    auto openTickets = db->Query("SELECT id FROM ticket2user WHERE ticket=" +
                                 ses.Param("elid") + " AND user IN (" +
                                 str::Join(allowedDepartments, ",") + ")");
    string elid;
    if (openTickets->Eof()) {
      if (ses.Param("type") == "setstatus" && ses.Param("status") == "closed") {
        ses.NewNode("ok");
        return;
      }
      if (retry) {
        InternalCall(ses, "support_tool_responsible",
                     "set_responsible_default=off&sok=ok&set_responsible=e%5F" +
                         allowedDepartments[0] + "&elid=" + ses.Param("elid"));
        Execute(ses, false);
        return;
      } else {
        throw mgr_err::Error("cannot_open_ticket");
      }
    } else {
      elid = openTickets->Str();
    }

    if (ses.Param("type") == "setstatus" && ses.Param("status") == "new") {
      return;
    }

    auto ret2 = InternalCall(
        ses, "ticket.edit",
        string() + "sok=ok&show_optional=on" + "&clicked_button=" +
            (ses.Param("status") == "new" ? "ok_message" : "ok") + "&" +
            (!ses.Checked("internal") ? "message" : "note_message") + "=" +
            str::url::Encode(ses.Param("message")) + "&elid=" + elid);
    // TODO: attachments, sender_name
    ses.NewNode("ok");
  }
};

struct TicketmgriLastNote : public mgr_db::CustomTable {
  mgr_db::ReferenceField Ticket;
  mgr_db::ReferenceField LastNote;

  TicketmgriLastNote()
      : mgr_db::CustomTable("ticketmgri_last_note"),
        Ticket(this, "ticket", mgr_db::rtRestrict),
        LastNote(this, "last_note", "ticket_note", mgr_db::rtRestrict) {
    Ticket.info().set_primary();
  }
};

struct aTicketintegrationLastNote : public Action {
  aTicketintegrationLastNote()
      : Action("ticketintegraion.last_note", MinLevel(lvSuper)) {}

  void Execute(Session &ses) const override {
    auto t = db->Get<TicketmgriLastNote>();
    if (!t->Find(ses.Param("elid"))) {
      t->New();
      t->Ticket = str::Int(ses.Param("elid"));
    }
    if (ses.IsSubmitted()) {
      t->LastNote = str::Int(ses.Param("last_note"));
      t->Post();
      ses.Ok();
    } else {
      ses.NewNode("last_note", t->LastNote);
    }
  }
};

struct aTicketintegrationPushTasks : public Action {
  aTicketintegrationPushTasks()
      : Action("ticketintegraion.push_tasks", MinLevel(lvSuper)) {}

  void Execute(Session &ses) const override {
    mgr_xml::XPath xpath =
        InternalCall("longtask", "filter=yes&state=err&queue=ticketmgri_sync")
            .GetNodes("//elem[queue='ticketmgri_sync' and status='err']");
    for (auto elem : xpath) {
      auto data = InternalCall("longtask.edit",
                               "elid=" + elem.FindNode("pidfile").Str());
      mgr_task::LongTask(data.GetNode("//realname"), data.GetNode("//id"),
                         "ticketmgri_sync")
          .SetParam(data.GetNode("//params"))
          .Start();
    }
  }
};

struct aTicketintegrationGetBalance : public Action {
  aTicketintegrationGetBalance()
      : Action("ticketintegration.getbalance", MinLevel(lvAdmin)) {}

  void Execute(Session &ses) const override {
    ses.NewNode("balance",
                InternalCall(ses, "account.edit", "elid=" + ses.Param("elid"))
                    .GetNode("//balance")
                    .Str());
  }

  bool IsModify(const Session &) const override { return false; }
};

string GetOpenTicket(const string &ticket) {
  auto openTickets =
      db->Query("SELECT id FROM ticket2user WHERE ticket=" + ticket +
                " AND user IN (" + str::Join(allowedDepartments, ",") + ")");
  if (openTickets->Eof()) {
    throw mgr_err::Value("ticket");
  }
  return openTickets->AsString(0);
}

struct aTicketintegrationDeduct : public Action {
  aTicketintegrationDeduct()
      : Action("ticketintegration.deduct", MinLevel(lvAdmin)) {}

  void Execute(Session &ses) const override {
    string elid = GetOpenTicket(ses.Param("ticket"));
    InternalCall(ses, "ticket.edit", "sok=ok&show_optional=on&elid=" + elid +
                                         "&ticket_expense=" +
                                         ses.Param("amount"));
    ses.NewNode("ok");
  }
};

struct aTicketintegrationSetDepartment : public Action {
  aTicketintegrationSetDepartment()
      : Action("ticketintegration.setdepartment", MinLevel(lvAdmin)) {}

  void Execute(Session &ses) const override {
    string ticket_id = ses.Param("elid");
    string elid = GetOpenTicket(ticket_id);
    string department = ses.Param("department");
    InternalCall(ses, "support_tool_responsible",
                 "sok=ok&set_responsible=d_" + department + "&elid=" + elid +
                     "&plid=" + ticket_id + "&set_responsible_default=on");
    if (std::find(allowedDepartments.begin(), allowedDepartments.end(),
                  department) == allowedDepartments.end()) {
      // openTicket must be closed for this user.
      InternalCall(ses, "ticketintegration.post",
                   "sok=ok&type=setstatus&status=closed&elid=" + ticket_id);
    }
    ses.NewNode("ok");
  }
};

}  // namespace

MODULE_INIT(ticketmgri, "") {
  Warning("Init TICKETmanager integtration");
  mgr_cf::AddParam("TicketmgrUrl",
                   "https://tickets.isplicense.ru:1500/ticketmgr");
  mgr_cf::AddParam("TicketmgrLogin");
  mgr_cf::AddParam("TicketmgrPassword");
  mgr_cf::AddParam("TicketmgrBillmgrUrl");
  mgr_cf::AddParam("TicketmgrUserId");
  mgr_cf::AddParam("TicketmgrAllowedDepartments");
  mgr_cf::AddParam("TicketmgrHideDepartments");
  str::Split(mgr_cf::GetParam("TicketmgrAllowedDepartments"), ",",
             allowedDepartments);
  if (allowedDepartments.empty()) {
    allowedDepartments.push_back("0");
  }
  str::Split(mgr_cf::GetParam("TicketmgrHideDepartments"), ",",
             hideDepartments);
  db->Register<TicketmgriLastNote>();
  new eClientTicketEdit;
  new eTicketEdit("ticket.edit", "elid_ticket2user");
  new eTicketEdit("support_tool_responsible", "plid");
  new aTicketintegrationSetFilter;
  new aTicketintegrationPost;
  new aTicketintegrationLastNote;
  new aTicketintegrationPushTasks;
  new aTicketintegrationGetBalance;
  new aTicketintegrationDeduct;
  new aTicketintegrationSetDepartment;
}
