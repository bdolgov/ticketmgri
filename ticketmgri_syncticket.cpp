#include <billmgr/db.h>
#include <billmgr/defines.h>
#include <billmgr/sbin_utils.h>
#include <ispbin.h>
#include <mgr/mgrclient.h>
#include <mgr/mgrdb_struct.h>
#include <mgr/mgrenv.h>
#include <mgr/mgrlog.h>
#include <mgr/mgrproc.h>
#include <mgr/mgrrpc.h>

MODULE("syncticket");

using sbin::DB;
using sbin::GetMgrConfParam;
using sbin::Client;
using sbin::ClientQuery;

mgr_client::Client &ticketmgr() {
  static mgr_client::Client *ret = []() {
    mgr_client::Remote *ret =
        new mgr_client::Remote(GetMgrConfParam("TicketmgrUrl"));
    ret->AddParam("authinfo", GetMgrConfParam("TicketmgrLogin") + ":" +
                                  GetMgrConfParam("TicketmgrPassword"));
    return ret;
  }();
  return *ret;
}

void PostTicket(const string &elid) {
  auto ticket = DB()->Query("SELECT * FROM ticket WHERE id=" + elid);
  if (ticket->Eof()) throw mgr_err::Missed("ticket");
  auto account = DB()->Query("SELECT * FROM account WHERE id=" +
                             ticket->AsString("account_client"));
  if (account->Eof()) throw mgr_err::Missed("account");
  auto user = DB()->Query("SELECT * FROM user WHERE account=" +
                          account->AsString("id") + " ORDER BY id LIMIT 1");
  if (user->Eof()) throw mgr_err::Missed("user");

  mgr_xml::Xml infoXml;
  auto info = infoXml.GetRoot();
  auto customer = info.AppendChild("customer");
  customer.AppendChild("id", account->AsString("id"));
  customer.AppendChild("name", account->AsString("name"));
  customer.AppendChild("email", user->AsString("email"));
  customer.AppendChild("phone", user->AsString("phone"));
  customer.AppendChild("link",
                       GetMgrConfParam("TicketmgrBillmgrUrl") +
                           "?startform=ticketintegration.setfilter&elid=" +
                           account->AsString("id"));

  if (!ticket->IsNull("item")) {
    auto item =
        DB()->Query("SELECT id, name, processingmodule FROM item WHERE id=" +
                    ticket->AsString("item"));
    if (item->Eof()) throw mgr_err::Missed("item");
    auto iteminfo = info.AppendChild("item");
    iteminfo.SetProp("selected", "yes");
    iteminfo.AppendChild("id", item->AsString("id"));
    iteminfo.AppendChild("name", item->AsString("name"));
    iteminfo.AppendChild("serverid", item->AsString("processingmodule"));
    ForEachQuery(DB(), "SELECT intname, value FROM itemparam WHERE item=" +
                           ticket->AsString("item"),
                 i) {
      if (i->AsString(0) == "ip") {
        iteminfo.AppendChild("ip", i->AsString(1));
      } else if (i->AsString(0) == "username") {
        iteminfo.AppendChild("login", i->AsString(1));
      } else if (i->AsString(0) == "password") {
        iteminfo.AppendChild("password", i->AsString(1));
      } else if (i->AsString(0) == "domain") {
        iteminfo.AppendChild("domain", i->AsString(1));
      }
    }
  }
  StringMap args = {{"remoteid", ticket->AsString("id")},
                    {"department", ticket->AsString("responsible")},
                    {"info", infoXml.Str()},
                    {"subject", ticket->AsString("name")}};

  ticketmgr().Query("func=clientticket.add&sok=ok", args);
}

int ISP_MAIN(int ac, char **av) {
  if (ac != 2) {
    fprintf(stderr, "Usage: ticketmgri_syncticket ID");
    return 1;
  }

  string elid = av[1];

  try {
    mgr_log::Init("ticketmgri");
    string status = "closed";
    int lastmessage = 0;

    string newStatus =
        DB()->Query("SELECT COUNT(*) FROM ticket2user WHERE ticket=" + elid +
                    " AND user IN (" +
                    GetMgrConfParam("TicketmgrAllowedDepartments") + ")")
                ->Int()
            ? "new"
            : "closed";
    bool inDepartment =
        DB()->Query("SELECT COUNT(*) FROM ticket WHERE id=" + elid +
                    " AND responsible IN (" +
                    GetMgrConfParam("TicketmgrAllowedDepartments") + ")")
            ->Int();
    if (newStatus != "new" && !inDepartment) {
      LogNote("Skip ticket %s: status=%s, inDepartment=%d", elid.c_str(),
              newStatus.c_str(), inDepartment);
      return 0;
    }
    try {
      auto r = ticketmgr().Query("func=clientticket.info&remoteid=?", elid);
      status = r.value("status");
      lastmessage = str::Int(r.value("lastmessage"));
    } catch (mgr_err::Error &e) {
      if (e.type() == "missed" && e.object() == "remoteid") {
        PostTicket(elid);
      } else {
        throw;
      }
    }

    int lastnote =
        str::Int(Client()
                     .Query("func=ticketintegraion.last_note&elid=" + elid)
                     .value("last_note"));

    auto msg = DB()->Query(
        string() +
        "SELECT ticket_message.id, user.realname AS username, user.level AS "
        "userlevel, message, 1 AS type, ticket_message.date_post " +
        "FROM ticket_message " + "JOIN user ON ticket_message.user=user.id " +
        "WHERE ticket_message.id > " + str::Str(lastmessage) + " " +
        "AND user != " + GetMgrConfParam("TicketmgrUserId") + " " +
        "AND ticket = " + elid + " " +
        "UNION "
        "SELECT ticket_note.id, user.realname AS username, user.level AS "
        "userlevel, note AS message, 2 AS type, ticket_note.date_post " +
        "FROM ticket_note " + "JOIN user ON ticket_note.user=user.id " +
        "WHERE ticket_note.id > " + str::Str(lastnote) + " " + "AND user != " +
        GetMgrConfParam("TicketmgrUserId") + " " + "AND ticket = " + elid +
        " " + "ORDER BY date_post");

    if (msg->Eof() && status != newStatus) {
      StringMap params = {
          {"remoteid", elid}, {"status", newStatus},
      };
      ticketmgr().Query(
          "func=clientticket.post&sok=ok&sender=staff&sender_name=System&type="
          "setstatus",
          params);
    } else {
      lastnote = 0;
      for (msg->First(); !msg->Eof(); msg->Next()) {
        StringMap params = {
            {"remoteid", elid},
            {"status", newStatus},
            {"sender_name", msg->AsString("username")},
            {"sender", msg->AsInt("userlevel") >= 28 ? "staff" : "client"},
            {"message", msg->AsString("message")},
        };

        int attachments = 0;

        if (msg->AsInt("type") == 1) {
          params["messageid"] = msg->AsString("id");
          ForEachQuery(
              DB(),
              "SELECT * FROM ticket_message_attach WHERE ticket_message=" +
                  msg->AsString("id"),
              attach) {
            string id = str::Str(attachments++);
            auto info =
                ClientQuery("func=ticket.file&elid=" + attach->AsString("id"));
            params["attachment_name_" + id] =
                info.xml.GetNode("//content/name").Str();
            params["attachment_content_" + id] = str::base64::Encode(
                mgr_file::Read(info.xml.GetNode("//content/data").Str()));
          }
        } else {
          lastnote = std::max(lastnote, msg->AsInt("id"));
          params["internal"] = "on";
        }
        params["attachments"] = str::Str(attachments);

        ticketmgr().Query("func=clientticket.post&sok=ok&type=message", params);
      }
      if (lastnote) {
        Client().Query("func=ticketintegraion.last_note&sok=ok&elid=" + elid +
                       "&last_note=" + str::Str(lastnote));
      }
    }
  } catch (std::exception &e) {
    fprintf(stderr, "%s\n", e.what());
    return 1;
  }
  return 0;
}
