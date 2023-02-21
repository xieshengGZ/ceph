import { Component, OnDestroy, OnInit } from '@angular/core';

import _ from 'lodash';
import { Observable, Subscription, timer } from 'rxjs';
import { take } from 'rxjs/operators';
import moment from 'moment';

import { ClusterService } from '~/app/shared/api/cluster.service';
import { ConfigurationService } from '~/app/shared/api/configuration.service';
import { HealthService } from '~/app/shared/api/health.service';
import { MgrModuleService } from '~/app/shared/api/mgr-module.service';
import { OsdService } from '~/app/shared/api/osd.service';
import { PrometheusService } from '~/app/shared/api/prometheus.service';
import { Promqls as queries } from '~/app/shared/enum/dashboard-promqls.enum';
import { Icons } from '~/app/shared/enum/icons.enum';
import { DashboardDetails } from '~/app/shared/models/cd-details';
import { Permissions } from '~/app/shared/models/permissions';
import { AlertmanagerAlert } from '~/app/shared/models/prometheus-alerts';
import { AuthStorageService } from '~/app/shared/services/auth-storage.service';
import {
  FeatureTogglesMap$,
  FeatureTogglesService
} from '~/app/shared/services/feature-toggles.service';
import { RefreshIntervalService } from '~/app/shared/services/refresh-interval.service';
import { SummaryService } from '~/app/shared/services/summary.service';

@Component({
  selector: 'cd-dashboard',
  templateUrl: './dashboard.component.html',
  styleUrls: ['./dashboard.component.scss']
})
export class DashboardComponent implements OnInit, OnDestroy {
  detailsCardData: DashboardDetails = {};
  osdSettingsService: any;
  osdSettings: any;
  interval = new Subscription();
  permissions: Permissions;
  enabledFeature$: FeatureTogglesMap$;
  color: string;
  capacityService: any;
  capacity: any;
  healthData$: Observable<Object>;
  prometheusAlerts$: Observable<AlertmanagerAlert[]>;

  isAlertmanagerConfigured = false;
  icons = Icons;
  showAlerts = false;
  flexHeight = true;
  simplebar = {
    autoHide: false
  };
  textClass: string;
  borderClass: string;
  alertType: string;
  alerts: AlertmanagerAlert[];
  crticialActiveAlerts: number;
  warningActiveAlerts: number;
  healthData: any;
  categoryPgAmount: Record<string, number> = {};
  totalPgs = 0;
  queriesResults: any = {
    USEDCAPACITY: '',
    IPS: '',
    OPS: '',
    READLATENCY: '',
    WRITELATENCY: '',
    READCLIENTTHROUGHPUT: '',
    WRITECLIENTTHROUGHPUT: '',
    RECOVERYBYTES: ''
  };
  timerGetPrometheusDataSub: Subscription;
  timerTime = 30000;
  readonly lastHourDateObject = {
    start: moment().unix() - 3600,
    end: moment().unix(),
    step: 12
  };

  constructor(
    private summaryService: SummaryService,
    private configService: ConfigurationService,
    private mgrModuleService: MgrModuleService,
    private clusterService: ClusterService,
    private osdService: OsdService,
    private authStorageService: AuthStorageService,
    private featureToggles: FeatureTogglesService,
    private healthService: HealthService,
    public prometheusService: PrometheusService,
    private refreshIntervalService: RefreshIntervalService
  ) {
    this.permissions = this.authStorageService.getPermissions();
    this.enabledFeature$ = this.featureToggles.get();
  }

  ngOnInit() {
    this.interval = this.refreshIntervalService.intervalData$.subscribe(() => {
      this.getHealth();
      this.triggerPrometheusAlerts();
      this.getCapacityCardData();
    });
    this.getPrometheusData(this.lastHourDateObject);
    this.getDetailsCardData();
  }

  ngOnDestroy() {
    this.interval.unsubscribe();
  }

  getHealth() {
    this.healthService.getMinimalHealth().subscribe((data: any) => {
      this.healthData = data;
    });
  }

  toggleAlertsWindow(type: string, isToggleButton: boolean = false) {
    if (isToggleButton) {
      this.showAlerts = !this.showAlerts;
      this.flexHeight = !this.flexHeight;
    } else if (
      !this.showAlerts ||
      (this.alertType === type && type !== 'danger') ||
      (this.alertType !== 'warning' && type === 'danger')
    ) {
      this.showAlerts = !this.showAlerts;
      this.flexHeight = !this.flexHeight;
    }

    type === 'danger' ? (this.alertType = 'critical') : (this.alertType = type);
    this.textClass = `text-${type}`;
    this.borderClass = `border-${type}`;
  }

  getDetailsCardData() {
    this.configService.get('fsid').subscribe((data) => {
      this.detailsCardData.fsid = data['value'][0]['value'];
    });
    this.mgrModuleService.getConfig('orchestrator').subscribe((data) => {
      const orchStr = data['orchestrator'];
      this.detailsCardData.orchestrator = orchStr.charAt(0).toUpperCase() + orchStr.slice(1);
    });
    this.summaryService.subscribe((summary) => {
      const version = summary.version.replace('ceph version ', '').split(' ');
      this.detailsCardData.cephVersion =
        version[0] + ' ' + version.slice(2, version.length).join(' ');
    });
  }

  getCapacityCardData() {
    this.osdSettingsService = this.osdService
      .getOsdSettings()
      .pipe(take(1))
      .subscribe((data: any) => {
        this.osdSettings = data;
      });
    this.capacityService = this.clusterService.getCapacity().subscribe((data: any) => {
      this.capacity = data;
    });
  }

  triggerPrometheusAlerts() {
    this.prometheusService.ifAlertmanagerConfigured(() => {
      this.isAlertmanagerConfigured = true;

      this.prometheusService.getAlerts().subscribe((alerts) => {
        this.alerts = alerts;
        this.crticialActiveAlerts = alerts.filter(
          (alert: AlertmanagerAlert) =>
            alert.status.state === 'active' && alert.labels.severity === 'critical'
        ).length;
        this.warningActiveAlerts = alerts.filter(
          (alert: AlertmanagerAlert) =>
            alert.status.state === 'active' && alert.labels.severity === 'warning'
        ).length;
      });
    });
  }

  getPrometheusData(selectedTime: any) {
    if (this.timerGetPrometheusDataSub) {
      this.timerGetPrometheusDataSub.unsubscribe();
    }
    this.timerGetPrometheusDataSub = timer(0, this.timerTime).subscribe(() => {
      selectedTime = this.updateTimeStamp(selectedTime);

      for (const queryName in queries) {
        if (queries.hasOwnProperty(queryName)) {
          const query = queries[queryName];
          let interval = selectedTime.step;

          if (query.includes('rate') && selectedTime.step < 20) {
            interval = 20;
          } else if (query.includes('rate')) {
            interval = selectedTime.step * 2;
          }

          const intervalAdjustedQuery = query.replace(/\[(.*?)\]/g, `[${interval}s]`);

          this.prometheusService
            .getPrometheusData({
              params: intervalAdjustedQuery,
              start: selectedTime['start'],
              end: selectedTime['end'],
              step: selectedTime['step']
            })
            .subscribe((data: any) => {
              if (data.result.length) {
                this.queriesResults[queryName] = data.result[0].values;
              }
            });
        }
      }
    });
  }

  private updateTimeStamp(selectedTime: any): any {
    let formattedDate = {};
    const date: number = selectedTime['start'] + this.timerTime / 1000;
    const dateNow: number = selectedTime['end'] + this.timerTime / 1000;
    formattedDate = {
      start: date,
      end: dateNow,
      step: selectedTime['step']
    };
    return formattedDate;
  }
}
