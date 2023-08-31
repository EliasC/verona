// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include "../lang.h"
#include "../lookup.h"

namespace verona
{
  struct Pending
  {
    size_t rc = 0;
    std::vector<Lookup> inherit;
    Nodes deps;
  };

  size_t type_substitution(NodeMap<Node>& bindings, Node& node)
  {
    // Substitutes inside of `node`, but not `node` itself.
    size_t changes = 0;

    for (auto child : *node)
    {
      while ((child->type() == FQType) &&
             ((child / Type)->type() == TypeParamName))
      {
        auto l = resolve_fq(child);
        auto it = bindings.find(l.def);

        if (it == bindings.end())
          break;

        auto bind = clone(it->second);

        if (bind->type() == Type)
          bind = bind / Type;

        node->replace(child, bind);
        child = bind;
      }

      changes += type_substitution(bindings, child);
    }

    return changes;
  }

  PassDef codereuse()
  {
    auto pending = std::make_shared<NodeMap<Pending>>();
    auto ready = std::make_shared<Nodes>();

    PassDef codereuse = {
      dir::topdown | dir::once,
      {
        T(Class)[Class]
            << (T(Ident)[Ident] * T(TypeParams) *
                (T(Inherit) << T(Type)[Inherit]) * T(TypePred) *
                T(ClassBody)[ClassBody]) >>
          ([=](Match& _) -> Node {
            auto cls = _(Class);
            auto& pend = (*pending)[cls];
            std::vector<Lookup> worklist;
            worklist.emplace_back(Lookup(_(Inherit)));

            while (!worklist.empty())
            {
              auto l = worklist.back();
              worklist.pop_back();

              // This should only happen in test code.
              if (!l.def)
                continue;

              if (l.def->type() == FQType)
              {
                auto ll = resolve_fq(l.def);
                ll.bindings.merge(l.bindings);
                worklist.push_back(ll);
              }
              else if (l.def->type().in({Type, TypeIsect}))
              {
                for (auto& t : *l.def)
                  worklist.emplace_back(l.make(t));
              }
              else if (l.def->type() == TypeAlias)
              {
                // Carry typeargs forward.
                worklist.emplace_back(l.make(l.def / Type));
              }
              else if (l.def->type() == Trait)
              {
                pend.inherit.push_back(l);
              }
              else if (l.def->type() == Class)
              {
                if ((l.def / Inherit / Inherit)->type() == Type)
                {
                  // Keep track of the inheritance graph.
                  (*pending)[l.def].deps.push_back(cls);
                  pend.rc++;
                }

                pend.inherit.push_back(l);
              }
            }

            if (pend.rc == 0)
              ready->push_back(cls);

            return NoChange;
          }),
      }};

    codereuse.post([=](Node) {
      size_t changes = 0;

      while (!ready->empty())
      {
        auto cls = ready->back();
        ready->pop_back();
        auto& pend = (*pending)[cls];
        auto body = cls / ClassBody;
        (cls / Inherit) = Inherit << DontCare;
        changes++;

        for (auto& from : pend.inherit)
        {
          for (auto f : *(from.def / ClassBody))
          {
            if (!f->type().in({FieldLet, FieldVar, Function}))
              continue;

            // Don't inherit functions without implementations.
            if ((f->type() == Function) && ((f / Block)->type() == DontCare))
              continue;

            // If we have an explicit version that conflicts, don't inherit.
            auto defs = cls->lookdown((f / Ident)->location());
            if (std::any_of(defs.begin(), defs.end(), [&](Node& def) -> bool {
                  return conflict(f, def);
                }))
              continue;

            // Clone an implicit version into classbody.
            f = clone(f);
            (f / Implicit) = Implicit;
            body << f;
            changes++;

            // Type substitution in the inherited member.
            changes += type_substitution(from.bindings, f);
          }
        }

        for (auto& dep : pend.deps)
        {
          if (--pending->at(dep).rc == 0)
            ready->push_back(dep);
        }
      }

      pending->clear();
      return changes;
    });

    return codereuse;
  }
}
