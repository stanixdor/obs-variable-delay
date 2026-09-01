import * as rootParams from "next/root-params";
import {notFound} from "next/navigation";
import {hasLocale} from "next-intl";
import {getRequestConfig} from "next-intl/server";

import {routing} from "./routing";

export default getRequestConfig(async ({locale}) => {
  const resolvedLocale = locale ?? (await rootParams.locale());

  if (!hasLocale(routing.locales, resolvedLocale)) {
    notFound();
  }

  return {
    locale: resolvedLocale,
    messages:
      resolvedLocale === "en"
        ? (await import("../../messages/en.json")).default
        : (await import("../../messages/es.json")).default,
  };
});
