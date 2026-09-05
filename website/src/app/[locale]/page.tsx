import {
  Apple,
  ArrowRight,
  AudioWaveform,
  Check,
  Code2,
  Cpu,
  Download,
  ExternalLink,
  Eye,
  Gauge,
  Languages,
  Layers3,
  MemoryStick,
  Monitor,
  Radio,
  RefreshCw,
  ShieldCheck,
  Terminal,
  TimerReset,
} from "lucide-react";
import {cacheLife} from "next/cache";
import {hasLocale} from "next-intl";
import {getTranslations} from "next-intl/server";
import {notFound} from "next/navigation";

import type {Locale} from "@/i18n/routing";
import {routing} from "@/i18n/routing";
import packageInfo from "../../../package.json";

const version = packageInfo.version;
const repositoryUrl = "https://github.com/stanixdor/obs-variable-delay";
const releaseUrl = `${repositoryUrl}/releases/latest`;
const releaseAssetBase = `${repositoryUrl}/releases/download/${version}`;

const featureIcons = [RefreshCw, AudioWaveform, Eye, MemoryStick, Layers3, ShieldCheck];

type Feature = {title: string; description: string};
type WorkflowStep = {number: string; title: string; description: string};

type PageProps = {
  params: Promise<{locale: string}>;
};

type LandingProps = {
  locale: Locale;
};

async function Landing({locale}: LandingProps) {
  "use cache";
  cacheLife("max");

  const t = await getTranslations({locale, namespace: "Landing"});
  const metadata = await getTranslations({locale, namespace: "Metadata"});
  const alternateLocale = locale === "en" ? "es" : "en";
  const features = t.raw("features") as Feature[];
  const workflow = t.raw("workflow") as WorkflowStep[];
  const performancePoints = t.raw("performancePoints") as string[];
  const requirements = t.raw("requirements") as string[];
  const guideUrl = `${repositoryUrl}/blob/main/docs/guide.${locale}.md`;
  const platformDownloads = [
    {
      name: t("platformMac"),
      detail: t("platformMacDetail"),
      asset: t("platformMacAsset"),
      href: `${releaseAssetBase}/obs-dynamic-delay-${version}-macos-universal.zip`,
      Icon: Apple,
    },
    {
      name: t("platformWindows"),
      detail: t("platformWindowsDetail"),
      asset: t("platformWindowsAsset"),
      href: `${releaseAssetBase}/obs-dynamic-delay-${version}-windows-x64.zip`,
      Icon: Monitor,
    },
    {
      name: t("platformLinux"),
      detail: t("platformLinuxDetail"),
      asset: t("platformLinuxAsset"),
      href: `${releaseAssetBase}/obs-dynamic-delay-${version}-x86_64-linux-gnu.deb`,
      Icon: Terminal,
    },
  ];
  const jsonLd = {
    "@context": "https://schema.org",
    "@type": "SoftwareApplication",
    name: metadata("productName"),
    description: metadata("description"),
    softwareVersion: version,
    applicationCategory: "MultimediaApplication",
    operatingSystem: "macOS 13+, Windows 10/11, Ubuntu 24.04",
    url: `https://www.obsdelay.com/${locale}`,
    downloadUrl: releaseUrl,
    codeRepository: repositoryUrl,
    license: "https://spdx.org/licenses/GPL-2.0-or-later.html",
    offers: {
      "@type": "Offer",
      price: "0",
      priceCurrency: "USD",
    },
  };

  return (
    <>
      <script
        type="application/ld+json"
        dangerouslySetInnerHTML={{__html: JSON.stringify(jsonLd).replace(/</g, "\\u003c")}}
      />
      <a className="skipLink" href="#main-content">{t("skipContent")}</a>
      <header className="siteHeader">
        <a className="brand" href="#top" aria-label={t("brand")}>
          <span className="brandMark" aria-hidden="true">
            <TimerReset size={19} strokeWidth={2.2} />
          </span>
          <span>{t("brand")}</span>
        </a>

        <nav className="desktopNav" aria-label={t("navLabel")}>
          <a href="#features">{t("navFeatures")}</a>
          <a href="#workflow">{t("navHow")}</a>
          <a href="#download">{t("navDownload")}</a>
        </nav>

        <div className="headerActions">
          <a className="languageLink" href={`/${alternateLocale}`} hrefLang={alternateLocale}>
            <Languages size={15} aria-hidden="true" />
            {t("language")}
          </a>
          <a className="iconLink" href={repositoryUrl} aria-label={t("github")}>
            <Code2 size={18} aria-hidden="true" />
          </a>
        </div>
      </header>

      <main id="main-content">
      <section className="hero" id="top">
        <div className="heroCopy">
          <div className="eyebrow">
            <span className="liveDot" aria-hidden="true" />
            {t("eyebrow", {version})}
          </div>
          <h1>
            <span>{t("headlineTop")}</span>
            <span className="accentHeadline">{t("headlineBottom")}</span>
          </h1>
          <p className="heroIntro">{t("intro")}</p>

          <div className="heroActions">
            <a className="button buttonPrimary" href={releaseUrl}>
              <Download size={18} aria-hidden="true" />
              {t("download")}
              <ArrowRight size={17} aria-hidden="true" />
            </a>
            <a className="button buttonSecondary" href={repositoryUrl}>
              <Code2 size={18} aria-hidden="true" />
              {t("viewSource")}
            </a>
          </div>

          <div className="heroMeta" role="group" aria-label={t("compatibilityLabel")}>
            <span>{t("platforms")}</span>
            <span>{t("openSource")}</span>
          </div>
        </div>

        <div className="heroVisual" aria-hidden="true">
          <div className="signalGrid" aria-hidden="true" />
          <div className="delayPanel">
            <div className="panelHeader">
              <div className="panelTitle">
                <Radio size={16} aria-hidden="true" />
                {t("panelTitle")}
              </div>
              <div className="liveStatus">
                <span />
                {t("panelLive")}
              </div>
            </div>

            <div className="panelBody">
              <div className="panelRow panelRowSplit">
                <span>{t("panelDelay")}</span>
                <strong>{t("panelSeconds")}</strong>
              </div>
              <div className="delaySlider" aria-hidden="true">
                <span className="delaySliderFill" />
                <span className="delaySliderThumb" />
              </div>
              <div className="panelStats">
                <div>
                  <span>{t("panelMemory")}</span>
                  <strong>28.4 MiB</strong>
                </div>
                <div>
                  <span>{t("panelScene")}</span>
                  <strong>{t("panelSceneValue")}</strong>
                </div>
              </div>
              <div className="mockButton">
                {t("panelButton")}
              </div>
              <div className="previewToggle">
                <span>{t("panelPreview")}</span>
                <span className="toggle" aria-hidden="true">
                  <span />
                </span>
              </div>
            </div>
          </div>

          <div className="timelineCard">
            <div className="timelineLabels">
              <span>{t("timelineNow")}</span>
              <span>{t("timelineBuffer")}</span>
              <span>{t("timelineAudience")}</span>
            </div>
            <div className="timelineRail" aria-hidden="true">
              <span className="timelineLive" />
              <span className="timelineBuffered" />
              <span className="timelineNode timelineNodeStart" />
              <span className="timelineNode timelineNodeEnd" />
            </div>
            <div className="signalLegend">
              <span><i className="legendLive" />{t("signalLive")}</span>
              <span><i className="legendDelayed" />{t("signalDelayed")}</span>
            </div>
          </div>
        </div>
      </section>

      <section className="proofBar" aria-label={t("proofLabel")}>
        <div className="proofItem">
          <strong>{t("proofDelayValue")}</strong>
          <span>{t("proofDelayLabel")}</span>
        </div>
        <div className="proofItem">
          <strong>{t("proofRestartValue")}</strong>
          <span>{t("proofRestartLabel")}</span>
        </div>
        <div className="proofItem">
          <strong>{t("proofPlatformsValue")}</strong>
          <span>{t("proofPlatformsLabel")}</span>
        </div>
        <div className="proofSignal" aria-hidden="true">
          <span />
          <span />
          <span />
          <span />
          <span />
          <span />
          <span />
          <span />
          <span />
          <span />
          <span />
          <span />
        </div>
      </section>

      <section className="sectionShell featuresSection" id="features" aria-labelledby="features-title">
        <div className="sectionHeading">
          <p className="sectionKicker">{t("featuresKicker")}</p>
          <h2 id="features-title">{t("featuresTitle")}</h2>
          <p>{t("featuresIntro")}</p>
        </div>

        <div className="featureGrid">
          {features.map((feature, index) => {
            const Icon = featureIcons[index];
            return (
              <article className="featureCard" key={feature.title}>
                <span className="featureIcon" aria-hidden="true">
                  <Icon size={20} strokeWidth={1.8} />
                </span>
                <span className="featureIndex">0{index + 1}</span>
                <h3>{feature.title}</h3>
                <p>{feature.description}</p>
              </article>
            );
          })}
        </div>
      </section>

      <section className="sectionShell workflowSection" id="workflow" aria-labelledby="workflow-title">
        <div className="sectionHeading sectionHeadingWide">
          <p className="sectionKicker">{t("workflowKicker")}</p>
          <h2 id="workflow-title">{t("workflowTitle")}</h2>
          <p>{t("workflowIntro")}</p>
        </div>

        <div className="workflowRail">
          {workflow.map((step, index) => (
            <article className="workflowStep" key={step.number}>
              <div className="workflowMarker">
                <span>{step.number}</span>
                {index < workflow.length - 1 && <i aria-hidden="true" />}
              </div>
              <div>
                <h3>{step.title}</h3>
                <p>{step.description}</p>
              </div>
            </article>
          ))}
        </div>
      </section>

      <section className="performanceSection" aria-labelledby="performance-title">
        <div className="performanceGlow" aria-hidden="true" />
        <div className="performanceCopy">
          <p className="sectionKicker">{t("performanceKicker")}</p>
          <h2 id="performance-title">{t("performanceTitle")}</h2>
          <p className="performanceBody">{t("performanceBody")}</p>
          <ul className="checkList">
            {performancePoints.map((point) => (
              <li key={point}>
                <Check size={16} aria-hidden="true" />
                <span>{point}</span>
              </li>
            ))}
          </ul>
        </div>

        <div className="performanceMetrics">
          <div className="metricCard metricCardMain">
            <Cpu size={21} aria-hidden="true" />
            <strong>{t("performanceMetricOne")}</strong>
            <span>{t("performanceMetricOneLabel")}</span>
            <div className="packetField" aria-hidden="true">
              {Array.from({length: 24}, (_, index) => <i key={index} />)}
            </div>
          </div>
          <div className="metricCard">
            <MemoryStick size={21} aria-hidden="true" />
            <strong>{t("performanceMetricTwo")}</strong>
            <span>{t("performanceMetricTwoLabel")}</span>
          </div>
          <div className="metricCard">
            <Gauge size={21} aria-hidden="true" />
            <strong>{t("performanceMetricThree")}</strong>
            <span>{t("performanceMetricThreeLabel")}</span>
          </div>
        </div>
      </section>

      <section className="sectionShell downloadSection" id="download" aria-labelledby="download-title">
        <div className="sectionHeading sectionHeadingWide">
          <p className="sectionKicker">{t("downloadKicker")}</p>
          <h2 id="download-title">{t("downloadTitle")}</h2>
          <p>{t("downloadIntro", {version})}</p>
        </div>

        <div className="downloadGrid">
          {platformDownloads.map(({name, detail, asset, href, Icon}, index) => (
            <article className="downloadCard" key={name}>
              <div className="downloadCardTop">
                <span className="platformIcon"><Icon size={25} strokeWidth={1.7} aria-hidden="true" /></span>
                {index === 0 && <span className="recommendedBadge">{t("recommended")}</span>}
              </div>
              <h3>{name}</h3>
              <p>{detail}</p>
              <a href={href} className="downloadLink">
                <span>
                  <Download size={17} aria-hidden="true" />
                  {t("downloadFor", {platform: name})}
                </span>
                <small>{asset}</small>
              </a>
            </article>
          ))}
        </div>

        <a className="allAssetsLink" href={releaseUrl}>
          {t("allAssets")}
          <ExternalLink size={15} aria-hidden="true" />
        </a>

        <div className="compatibilityGrid">
          <div className="compatibilityCard">
            <h3>{t("requirementsTitle")}</h3>
            <ul className="checkList compactList">
              {requirements.map((requirement) => (
                <li key={requirement}>
                  <Check size={15} aria-hidden="true" />
                  <span>{requirement}</span>
                </li>
              ))}
            </ul>
          </div>
          <div className="transparencyCard">
            <span className="transparencyIcon"><ShieldCheck size={22} aria-hidden="true" /></span>
            <h3>{t("transparencyTitle")}</h3>
            <p>{t("transparencyBody")}</p>
            <a href={guideUrl}>
              {t("transparencyLink")}
              <ArrowRight size={15} aria-hidden="true" />
            </a>
          </div>
        </div>
      </section>

      <section className="ctaSection" aria-labelledby="cta-title">
        <div className="ctaSignal" aria-hidden="true" />
        <p className="sectionKicker">{t("ctaKicker")}</p>
        <h2 id="cta-title">{t("ctaTitle")}</h2>
        <p>{t("ctaBody")}</p>
        <div className="heroActions ctaActions">
          <a className="button buttonPrimary" href={releaseUrl}>
            <Download size={18} aria-hidden="true" />
            {t("ctaDownload", {version})}
          </a>
          <a className="button buttonSecondary" href={repositoryUrl}>
            <Code2 size={18} aria-hidden="true" />
            {t("ctaSource")}
          </a>
        </div>
      </section>
      </main>

      <footer className="siteFooter">
        <div className="footerBrand">
          <span className="brandMark" aria-hidden="true"><TimerReset size={19} strokeWidth={2.2} /></span>
          <div>
            <strong>{t("brand")}</strong>
            <p>{t("footerDescription")}</p>
          </div>
        </div>
        <div className="footerLinks">
          <a href={guideUrl}>{t("footerGuide")}</a>
          <a href={releaseUrl}>{t("footerRelease")}</a>
          <a href={`${repositoryUrl}/blob/main/LICENSE`}>{t("footerLicense")}</a>
        </div>
        <p className="footerBuilt">{t("footerBuilt")}</p>
      </footer>
    </>
  );
}

export default async function HomePage({params}: PageProps) {
  const {locale} = await params;

  if (!hasLocale(routing.locales, locale)) {
    notFound();
  }

  return <Landing locale={locale} />;
}
